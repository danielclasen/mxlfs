// SPDX-License-Identifier: GPL-2.0-only
#include <linux/capability.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/stringhash.h>

#include "mxlfs.h"

enum { MXLFS_WANT_PENDING, MXLFS_WANT_APPEARED, MXLFS_WANT_FAILED };

/* A thread group that made entries in the wanted list; under wanted_lock. */
struct mxlfs_wanted_owner {
	struct hlist_node node;
	struct pid *tgid;
	unsigned int count;	/* its entries */
};

/* A missing flow that path walks are waiting for; under wanted_lock. */
struct mxlfs_wanted {
	struct list_head node;
	struct hlist_node hash;
	struct inode *dir;
	char *name;
	unsigned int len;
	char *path;		/* from the filesystem root */
	u64 id;
	struct mxlfs_wanted_owner *owner;	/* first waiter's thread group */
	unsigned int waiters;
	bool reported;
	int state;
	int error;
	wait_queue_head_t wq;
};

static u32 mxlfs_wanted_key(const struct inode *dir, const struct qstr *name)
{
	return full_name_hash(dir, name->name, name->len);
}

/*
 * The pending entry for name in dir. An entry whose flow appeared or was
 * failed only waits for its waiters to leave; a walk joining it would
 * take that outcome for its own, so such an entry is not found and a
 * walk that still sees the name missing makes a new one.
 */
static struct mxlfs_wanted *mxlfs_wanted_find(struct mxlfs_sb_info *sbi,
					      struct inode *dir,
					      const struct qstr *name)
{
	struct mxlfs_wanted *w;

	lockdep_assert_held(&sbi->wanted_lock);
	hash_for_each_possible(sbi->wanted_tables->names, w, hash,
			       mxlfs_wanted_key(dir, name))
		if (w->state == MXLFS_WANT_PENDING && w->dir == dir &&
		    w->len == name->len && !memcmp(w->name, name->name, name->len))
			return w;
	return NULL;
}

static struct mxlfs_wanted_owner *mxlfs_wanted_owner(struct mxlfs_sb_info *sbi,
						     struct pid *tgid)
{
	struct mxlfs_wanted_owner *o;

	lockdep_assert_held(&sbi->wanted_lock);
	hash_for_each_possible(sbi->wanted_tables->owners, o, node, (unsigned long)tgid)
		if (o->tgid == tgid)
			return o;
	return NULL;
}

/* Whether another entry by o's thread group would pass a cap. */
static bool mxlfs_wanted_full(struct mxlfs_sb_info *sbi, struct mxlfs_wanted_owner *o)
{
	lockdep_assert_held(&sbi->wanted_lock);
	return sbi->wanted_count >= MXLFS_WANTED_MAX ||
	       (o && o->count >= MXLFS_WANTED_MAX_TGID);
}

/* Also frees an entry never listed, whose dir is NULL. */
static void mxlfs_wanted_free(struct mxlfs_wanted *w)
{
	iput(w->dir);
	kfree(w->name);
	kfree(w->path);
	kfree(w);
}

/*
 * A new entry for dentry, with its name and path, not yet listed. The
 * PATH_MAX scratch buffer is freed before the walk waits. A path
 * MXLFS_IOC_WANTED_NEXT cannot hold fails the walk with ENAMETOOLONG
 * rather than make it wait for a flow no daemon learns of.
 */
static struct mxlfs_wanted *mxlfs_wanted_new(struct dentry *dentry)
{
	struct mxlfs_wanted *w;
	char *buf, *path;
	int err = -ENOMEM;

	w = kzalloc(sizeof(*w), GFP_KERNEL_ACCOUNT);
	if (!w)
		return ERR_PTR(-ENOMEM);
	buf = __getname();
	if (buf) {
		path = dentry_path_raw(dentry, buf, PATH_MAX);
		if (IS_ERR(path))
			err = PTR_ERR(path);
		else if (strlen(path) >= sizeof_field(struct mxlfs_wanted_req, path))
			err = -ENAMETOOLONG;
		else
			w->path = kstrdup(path, GFP_KERNEL_ACCOUNT);
		__putname(buf);
	}
	w->name = kmemdup(dentry->d_name.name, dentry->d_name.len, GFP_KERNEL_ACCOUNT);
	if (!w->path || !w->name) {
		mxlfs_wanted_free(w);
		return ERR_PTR(err);
	}
	w->len = dentry->d_name.len;
	init_waitqueue_head(&w->wq);
	return w;
}

/* Wakes the walks waiting for name in dir, which now exists. */
void mxlfs_wanted_appeared(struct inode *dir, const struct qstr *name)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(dir->i_sb);
	struct mxlfs_wanted *w;

	if (!sbi->wait_ms || !mxlfs_is_flow_name(name))
		return;
	spin_lock(&sbi->wanted_lock);
	w = mxlfs_wanted_find(sbi, dir, name);
	if (w) {
		w->state = MXLFS_WANT_APPEARED;
		wake_up_all(&w->wq);
	}
	spin_unlock(&sbi->wanted_lock);
}

static int mxlfs_wait_for(struct dentry *dentry)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(dentry->d_sb);
	unsigned long timeout = msecs_to_jiffies(sbi->wait_ms);
	struct mxlfs_wanted_owner *o, *no = NULL, *gone = NULL;
	struct pid *tgid = task_tgid(current);
	struct mxlfs_wanted *w, *nw = NULL;
	struct dentry *parent;
	struct inode *dir;
	int state, error, ret = 0;
	bool last, moved = false;
	long left;

	if (!timeout)
		return 0;

	parent = dget_parent(dentry);
	dir = d_inode(parent);
	/*
	 * A walk that joins an entry, or is turned away, allocates nothing.
	 * One that makes an entry, or the first of its thread group, drops
	 * the lock to allocate what it lacks and looks again: another walk
	 * may have made the entry, filled a cap or emptied the thread
	 * group's record meanwhile. Each pass allocates what is missing, so
	 * the third pass at the latest decides. A dentry the flow has moved
	 * onto or made positive since the walk found it gets no entry: the
	 * rename or create took wanted_lock after changing it, so either
	 * this sees the change or that finds the entry.
	 */
	spin_lock(&sbi->wanted_lock);
	for (;;) {
		w = mxlfs_wanted_find(sbi, dir, &dentry->d_name);
		if (w)
			break;
		moved = d_unhashed(dentry) || d_really_is_positive(dentry);
		if (moved)
			break;
		o = mxlfs_wanted_owner(sbi, tgid);
		if (mxlfs_wanted_full(sbi, o))
			break;
		if (nw && (o || no))
			break;
		spin_unlock(&sbi->wanted_lock);
		if (!nw) {
			nw = mxlfs_wanted_new(dentry);
			if (IS_ERR(nw)) {
				ret = PTR_ERR(nw);
				nw = NULL;
				goto out;
			}
		}
		if (!o && !no) {
			no = kmalloc(sizeof(*no), GFP_KERNEL_ACCOUNT);
			if (!no) {
				ret = -ENOMEM;
				goto out;
			}
		}
		spin_lock(&sbi->wanted_lock);
	}
	/* Not full, the loop left with what a new entry needs. */
	if (!w && !moved && !mxlfs_wanted_full(sbi, o)) {
		if (!o) {
			o = no;
			no = NULL;
			o->tgid = get_pid(tgid);
			o->count = 0;
			hash_add(sbi->wanted_tables->owners, &o->node, (unsigned long)tgid);
		}
		o->count++;
		w = nw;
		nw = NULL;
		w->owner = o;
		w->dir = igrab(dir);
		w->id = ++sbi->wanted_seq;
		list_add_tail(&w->node, &sbi->wanted);
		hash_add(sbi->wanted_tables->names, &w->hash,
			 mxlfs_wanted_key(dir, &dentry->d_name));
		WRITE_ONCE(sbi->wanted_count, sbi->wanted_count + 1);
		wake_up_interruptible(&sbi->wanted_wq);
	}
	if (w)
		w->waiters++;
	spin_unlock(&sbi->wanted_lock);
	if (moved) {
		ret = d_really_is_positive(dentry) ? 0 : -ESTALE;
	} else if (!w) {
		atomic64_inc(&sbi->wanted_refused);
		ret = -ENOENT;
	}
out:
	dput(parent);
	kfree(no);
	if (nw)
		mxlfs_wanted_free(nw);
	if (!w)
		return ret;

	/*
	 * A rename onto the name unhashes this negative dentry and a create
	 * in place makes it positive, each before it wakes the entry; which
	 * of them happened decides the result below.
	 */
	left = wait_event_killable_timeout(w->wq,
					   READ_ONCE(w->state) != MXLFS_WANT_PENDING ||
					   d_unhashed(dentry) || d_really_is_positive(dentry),
					   timeout);

	spin_lock(&sbi->wanted_lock);
	state = w->state;
	error = w->error;
	last = --w->waiters == 0;
	if (last) {
		list_del(&w->node);
		hash_del(&w->hash);
		WRITE_ONCE(sbi->wanted_count, sbi->wanted_count - 1);
		if (!--w->owner->count) {
			gone = w->owner;
			hash_del(&gone->node);
		}
	}
	spin_unlock(&sbi->wanted_lock);
	if (last)
		mxlfs_wanted_free(w);
	if (gone) {
		put_pid(gone->tgid);
		kfree(gone);
	}

	if (d_really_is_positive(dentry))
		return 0;
	if (d_unhashed(dentry) || state == MXLFS_WANT_APPEARED)
		return -ESTALE;	/* the walk is retried and finds the flow */
	if (left < 0)
		return left;
	if (state == MXLFS_WANT_FAILED)
		return error;
	return -ENOENT;
}

static int mxlfs_d_manage(const struct path *path, bool rcu)
{
	struct dentry *dentry = path->dentry;

	if (d_really_is_positive(dentry))
		return 0;
	if (rcu)
		return -ECHILD;
	return mxlfs_wait_for(dentry);
}

const struct dentry_operations mxlfs_dentry_operations = {
	.d_manage	= mxlfs_d_manage,
};

/* Copies the oldest flow not yet reported into req and marks it reported. */
static bool mxlfs_wanted_take(struct mxlfs_sb_info *sbi, struct mxlfs_wanted_req *req)
{
	struct mxlfs_wanted *w;
	bool found = false;

	spin_lock(&sbi->wanted_lock);
	list_for_each_entry(w, &sbi->wanted, node) {
		if (w->reported || w->state != MXLFS_WANT_PENDING)
			continue;
		w->reported = true;
		req->id = w->id;
		/* As the caller sees it; 0 when outside its pid namespace. */
		req->pid = pid_vnr(w->owner->tgid);
		strscpy(req->path, w->path, sizeof(req->path));
		found = true;
		break;
	}
	spin_unlock(&sbi->wanted_lock);
	return found;
}

/*
 * Returns flow id to those not yet reported, after its copy to the caller
 * failed, and wakes the callers waiting for one. Another caller has not
 * taken it meanwhile, since it was marked; it may have appeared, been
 * failed or left the list, and then there is nothing to report.
 */
static void mxlfs_wanted_unreport(struct mxlfs_sb_info *sbi, u64 id)
{
	struct mxlfs_wanted *w;

	spin_lock(&sbi->wanted_lock);
	list_for_each_entry(w, &sbi->wanted, node) {
		if (w->id != id)
			continue;
		w->reported = false;
		wake_up_interruptible(&sbi->wanted_wq);
		break;
	}
	spin_unlock(&sbi->wanted_lock);
}

long mxlfs_wanted_next(struct file *file, struct mxlfs_wanted_req __user *arg)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(file_inode(file)->i_sb);
	struct mxlfs_wanted_req *req;
	u32 flags, timeout_ms;
	long ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (get_user(flags, &arg->flags))
		return -EFAULT;
	if (flags)
		return -EINVAL;
	if (get_user(timeout_ms, &arg->timeout_ms))
		return -EFAULT;
	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;
	/* Written back whole: the inputs as passed, so the struct can be reused. */
	req->timeout_ms = timeout_ms;
	ret = wait_event_interruptible_timeout(sbi->wanted_wq,
					       mxlfs_wanted_take(sbi, req),
					       msecs_to_jiffies(timeout_ms));
	if (!ret) {
		ret = -ETIMEDOUT;
	} else if (ret > 0 && copy_to_user(arg, req, sizeof(*req))) {
		mxlfs_wanted_unreport(sbi, req->id);
		ret = -EFAULT;
	} else if (ret > 0) {
		ret = 0;
	}
	kfree(req);
	return ret;
}

long mxlfs_wanted_fail(struct file *file, struct mxlfs_wanted_fail __user *arg)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(file_inode(file)->i_sb);
	struct mxlfs_wanted_fail req;
	struct mxlfs_wanted *w;
	long ret = -ENOENT;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	/* As fuse_dev_do_write: ERESTARTSYS (512) and up are kernel-internal. */
	if (req.error <= 0 || req.error >= ERESTARTSYS || req.pad)
		return -EINVAL;
	spin_lock(&sbi->wanted_lock);
	list_for_each_entry(w, &sbi->wanted, node) {
		if (w->id != req.id)
			continue;
		if (w->state == MXLFS_WANT_PENDING) {
			w->state = MXLFS_WANT_FAILED;
			w->error = -req.error;
			wake_up_all(&w->wq);
		}
		ret = 0;
		break;
	}
	spin_unlock(&sbi->wanted_lock);
	return ret;
}
