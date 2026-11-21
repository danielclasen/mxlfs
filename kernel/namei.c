// SPDX-License-Identifier: GPL-2.0-only
#include <linux/namei.h>
#include <linux/security.h>

#include "mxlfs.h"

static void mxlfs_clear_managed(struct dentry *dentry)
{
	spin_lock(&dentry->d_lock);
	dentry->d_flags &= ~DCACHE_MANAGE_TRANSIT;
	spin_unlock(&dentry->d_lock);
}

static struct dentry *mxlfs_lookup(struct inode *dir, struct dentry *dentry,
				   unsigned int flags)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(dir->i_sb);

	if (dentry->d_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);
	/* A create or rename target must see the name as absent, at once. */
	if (sbi->wait_ms && mxlfs_is_flow_name(&dentry->d_name) &&
	    !(flags & (LOOKUP_CREATE | LOOKUP_EXCL | LOOKUP_RENAME_TARGET))) {
		/*
		 * The VFS fails a RESOLVE_NO_XDEV walk on a managed dentry with
		 * EXDEV, as for a mount it may not cross, without calling
		 * ->d_manage. Such a walk does not wait: the dentry is left
		 * unhashed, as simple_lookup leaves one in a casefolded
		 * directory, so the walk fails with ENOENT, and no other walk
		 * finds the name hashed without the flag and fails at once.
		 * Managed dentries are not kept once no walk holds them
		 * (s_d_flags), so such a walk meets one, and EXDEV, only
		 * while another walk holds the name.
		 */
		if (flags & LOOKUP_NO_XDEV)
			return NULL;
		spin_lock(&dentry->d_lock);
		dentry->d_flags |= DCACHE_MANAGE_TRANSIT;
		spin_unlock(&dentry->d_lock);
	}
	d_add(dentry, NULL);
	return NULL;
}

static int mxlfs_mknod(MXLFS_IDMAP *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode *inode = mxlfs_get_inode(dir->i_sb, dir, mode);
	int error;

	if (!inode)
		return -ENOSPC;
	if (!S_ISREG(mode) && !S_ISDIR(mode) && !S_ISLNK(mode))
		init_special_inode(inode, mode, dev);

	error = security_inode_init_security(inode, dir, &dentry->d_name,
					     NULL, NULL);
	if (error) {
		iput(inode);
		return error;
	}
	mxlfs_clear_managed(dentry);
	mxlfs_d_pin(dentry, inode); /* pin the dentry, as ramfs does */
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mxlfs_wanted_appeared(dir, &dentry->d_name);
	return 0;
}

static struct dentry *mxlfs_mkdir(MXLFS_IDMAP *idmap, struct inode *dir,
				  struct dentry *dentry, umode_t mode)
{
	int error = mxlfs_mknod(idmap, dir, dentry, mode | S_IFDIR, 0);

	if (!error)
		inc_nlink(dir);
	return ERR_PTR(error);
}

#if MXLFS_CREATE_HAS_EXCL
static int mxlfs_create(MXLFS_IDMAP *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
#else
static int mxlfs_create(MXLFS_IDMAP *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode)
#endif
{
	return mxlfs_mknod(idmap, dir, dentry, mode | S_IFREG, 0);
}

static int mxlfs_symlink(MXLFS_IDMAP *idmap, struct inode *dir,
			 struct dentry *dentry, const char *symname)
{
	struct inode *inode;
	int error;

	inode = mxlfs_get_inode(dir->i_sb, dir, S_IFLNK | 0777);
	if (!inode)
		return -ENOSPC;
	error = security_inode_init_security(inode, dir, &dentry->d_name,
					     NULL, NULL);
	if (!error)
		error = page_symlink(inode, symname, strlen(symname) + 1);
	if (error) {
		iput(inode);
		return error;
	}
	mxlfs_clear_managed(dentry);
	mxlfs_d_pin(dentry, inode);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mxlfs_wanted_appeared(dir, &dentry->d_name);
	return 0;
}

static int mxlfs_tmpfile(MXLFS_IDMAP *idmap, struct inode *dir,
			 struct file *file, umode_t mode)
{
	struct inode *inode;
	int error;

	inode = mxlfs_get_inode(dir->i_sb, dir, mode);
	if (!inode)
		return -ENOSPC;
	error = security_inode_init_security(inode, dir,
					     &file_dentry(file)->d_name,
					     NULL, NULL);
	if (error)
		iput(inode);
	else
		d_tmpfile(file, inode);
	return finish_open_simple(file, error);
}

static int mxlfs_link(struct dentry *old_dentry, struct inode *dir,
		      struct dentry *dentry)
{
	int error;

	mxlfs_clear_managed(dentry);
	error = simple_link(old_dentry, dir, dentry);
	if (!error)
		mxlfs_wanted_appeared(dir, &dentry->d_name);
	return error;
}

static int mxlfs_rename(MXLFS_IDMAP *idmap, struct inode *old_dir,
			struct dentry *old_dentry, struct inode *new_dir,
			struct dentry *new_dentry, unsigned int flags)
{
	struct qstr name = QSTR_LEN(new_dentry->d_name.name, new_dentry->d_name.len);
	char buf[NAME_MAX + 1];
	int error;

	/* d_move gives new_dentry the old name; keep the one being created. */
	memcpy(buf, name.name, name.len);
	name.name = buf;
	error = simple_rename(idmap, old_dir, old_dentry, new_dir, new_dentry, flags);
	if (error)
		return error;
	/*
	 * A woken walk retries once, and the VFS moves the dentries only
	 * after this returns. Left hashed, the negative target would meet
	 * that retry, which would wait again or, joining waiters not yet
	 * gone, fail with ESTALE. Unhashed, the retry looks the name up
	 * under new_dir's lock, held until the move is done.
	 */
	if (d_really_is_negative(new_dentry))
		d_drop(new_dentry);
	mxlfs_wanted_appeared(new_dir, &name);
	return 0;
}

const struct inode_operations mxlfs_dir_inode_operations = {
	.create		= mxlfs_create,
	.lookup		= mxlfs_lookup,
	.link		= mxlfs_link,
	.unlink		= simple_unlink,
	.symlink	= mxlfs_symlink,
	.mkdir		= mxlfs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= mxlfs_mknod,
	.rename		= mxlfs_rename,
	.tmpfile	= mxlfs_tmpfile,
};

const struct file_operations mxlfs_dir_operations = {
	.open		= dcache_dir_open,
	.release	= dcache_dir_close,
	.llseek		= dcache_dir_lseek,
	.read		= generic_read_dir,
	.iterate_shared	= dcache_readdir,
	.fsync		= noop_fsync,
	.unlocked_ioctl	= mxlfs_dir_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};
