// SPDX-License-Identifier: GPL-2.0-only
#include <linux/backing-dev.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/magic.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "mxlfs.h"

#define MXLFS_DEFAULT_MODE 0755

static struct kmem_cache *mxlfs_inode_cachep;

struct inode *mxlfs_get_inode(struct super_block *sb,
			      const struct inode *dir, umode_t mode, dev_t dev)
{
	struct inode *inode = new_inode(sb);

	if (!inode)
		return NULL;

	inode->i_ino = get_next_ino();
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_mapping->a_ops = &mxlfs_aops;
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_unevictable(inode->i_mapping);
#ifdef CONFIG_CGROUP_WRITEBACK
	/*
	 * Shrinking a file splits the large folio across the new size, and
	 * the split accounts the dropped part of a dirty folio as cleaned
	 * against inode_to_wb(), which is i_wb. On a bdi without writeback
	 * only marking the inode dirty sets i_wb, and a file that was
	 * created, fallocated and written through a mapping need not have
	 * been, so the split would dereference NULL. This is the wb that
	 * __inode_attach_wb() picks on such a bdi; it holds no reference,
	 * and eviction detaches it.
	 */
	inode->i_wb = &inode_to_bdi(inode)->wb;
#endif
	simple_inode_init_ts(inode);

	/* Symlinks keep their target in page cache too. */
	MXLFS_I(inode)->node_pages = kcalloc(nr_node_ids, sizeof(atomic_long_t),
					     GFP_KERNEL_ACCOUNT);
	if (!MXLFS_I(inode)->node_pages) {
		iput(inode);
		return NULL;
	}

	switch (mode & S_IFMT) {
	case S_IFREG:
		mapping_set_large_folios(inode->i_mapping);
		inode->i_op = &mxlfs_file_inode_operations;
		inode->i_fop = &mxlfs_file_operations;
		break;
	case S_IFDIR:
		inode->i_op = &mxlfs_dir_inode_operations;
		inode->i_fop = &mxlfs_dir_operations;
		inc_nlink(inode);
		break;
	case S_IFLNK:
		inode->i_op = &page_symlink_inode_operations;
		inode_nohighmem(inode);
		break;
	default:
		init_special_inode(inode, mode, dev);
		break;
	}
	return inode;
}

static struct inode *mxlfs_alloc_inode(struct super_block *sb)
{
	struct mxlfs_inode *mi = alloc_inode_sb(sb, mxlfs_inode_cachep, GFP_KERNEL);

	if (!mi)
		return NULL;
	atomic_long_set(&mi->charged, 0);
	mi->lazy = false;
	atomic_set(&mi->view_users, 0);
	mi->fold_start = 0;
	mi->fold_end = 0;
	mi->view = NULL;
	mi->node_pages = NULL;
	mi->pref = NUMA_NO_NODE;
	atomic_set(&mi->holds, 0);
	return &mi->vfs_inode;
}

static void mxlfs_free_inode(struct inode *inode)
{
	kfree(MXLFS_I(inode)->node_pages);
	kmem_cache_free(mxlfs_inode_cachep, MXLFS_I(inode));
}

static void mxlfs_evict_inode(struct inode *inode)
{
	struct mxlfs_inode *mi = MXLFS_I(inode);
	unsigned int nid;

	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	mxlfs_settle(inode);
	for (nid = 0; mi->node_pages && nid < nr_node_ids; nid++)
		mxlfs_count(inode, nid, -atomic_long_read(&mi->node_pages[nid]));
	if (mi->view)
		mxlfs_view_free(mi->view);
}

static int mxlfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(dentry->d_sb);
	u64 used = atomic64_read(&sbi->allocated) >> PAGE_SHIFT;
	int err = simple_statfs(dentry, buf);

	if (err)
		return err;
	/* Unlimited reports all RAM, so capacity is never read as 0. */
	buf->f_blocks = sbi->limit ? sbi->limit >> PAGE_SHIFT : totalram_pages();
	buf->f_bfree = buf->f_blocks > used ? buf->f_blocks - used : 0;
	buf->f_bavail = buf->f_bfree;
	return 0;
}

static int mxlfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(root->d_sb);

	if (sbi->mode != MXLFS_DEFAULT_MODE)
		seq_printf(m, ",mode=%o", sbi->mode);
	if (sbi->huge == MXLFS_HUGE_ALWAYS)
		seq_puts(m, ",huge=always");
	if (sbi->numa_node != NUMA_NO_NODE)
		seq_printf(m, ",numa_node=%d", sbi->numa_node);
	seq_printf(m, ",size=%llu", sbi->limit);
	if (sbi->wait_ms)
		seq_printf(m, ",wait=%u", sbi->wait_ms);
	return 0;
}

static const struct super_operations mxlfs_ops = {
	.alloc_inode	= mxlfs_alloc_inode,
	.free_inode	= mxlfs_free_inode,
	.evict_inode	= mxlfs_evict_inode,
	.statfs		= mxlfs_statfs,
	.drop_inode	= inode_just_drop,
	.show_options	= mxlfs_show_options,
};

enum { Opt_mode, Opt_huge, Opt_numa_node, Opt_size, Opt_wait };

static const struct constant_table mxlfs_param_huge[] = {
	{ "within_size", MXLFS_HUGE_WITHIN_SIZE },
	{ "always", MXLFS_HUGE_ALWAYS },
	{}
};

static const struct fs_parameter_spec mxlfs_fs_parameters[] = {
	fsparam_u32oct("mode", Opt_mode),
	fsparam_enum("huge", Opt_huge, mxlfs_param_huge),
	fsparam_u32("numa_node", Opt_numa_node),
	fsparam_string("size", Opt_size),
	fsparam_u32("wait", Opt_wait),
	{}
};

static int mxlfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct mxlfs_sb_info *sbi = fc->s_fs_info;
	struct fs_parse_result result;
	char *rest;
	int opt;

	opt = fs_parse(fc, mxlfs_fs_parameters, param, &result);
	if (opt < 0)
		return opt;
	sbi->seen |= BIT(opt);

	switch (opt) {
	case Opt_mode:
		sbi->mode = result.uint_32 & S_IALLUGO;
		break;
	case Opt_huge:
		sbi->huge = result.uint_32;
		break;
	case Opt_numa_node:
		/* Whether it has memory is a mount's question; see mxlfs_get_tree. */
		if (result.uint_32 >= nr_node_ids)
			return invalfc(fc, "numa_node=%u is no node", result.uint_32);
		sbi->numa_node = result.uint_32;
		break;
	case Opt_size:
		sbi->limit = memparse(param->string, &rest);
		if (*rest)
			return invalfc(fc, "Bad size: %s", param->string);
		break;
	case Opt_wait:
		sbi->wait_ms = result.uint_32;
		break;
	}
	return 0;
}

static int mxlfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct mxlfs_sb_info *sbi = sb->s_fs_info;
	struct inode *inode;

	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_blocksize		= PAGE_SIZE;
	sb->s_blocksize_bits	= PAGE_SHIFT;
	sb->s_magic		= MXLFS_MAGIC;
	sb->s_op		= &mxlfs_ops;
	set_default_d_op(sb, &mxlfs_dentry_operations);
	sb->s_d_flags		|= DCACHE_DONTCACHE;
	sb->s_time_gran		= 1;

	if (sbi->wait_ms) {
		/* The mount's, like its superblock info: not the mounter's cgroup. */
		sbi->wanted_tables = kvmalloc(sizeof(*sbi->wanted_tables), GFP_KERNEL);
		if (!sbi->wanted_tables)
			return -ENOMEM;
		hash_init(sbi->wanted_tables->names);
		hash_init(sbi->wanted_tables->owners);
	}
	inode = mxlfs_get_inode(sb, NULL, S_IFDIR | sbi->mode, 0);
	sb->s_root = d_make_root(inode);
	return sb->s_root ? 0 : -ENOMEM;
}

/*
 * A mount's numa_node= must have memory. A remount repeating it is not
 * asked again: the node may have lost its memory since, which the mount
 * already copes with by allocating elsewhere.
 */
static int mxlfs_get_tree(struct fs_context *fc)
{
	struct mxlfs_sb_info *sbi = fc->s_fs_info;

	if (sbi->numa_node != NUMA_NO_NODE && !mxlfs_node_valid(sbi->numa_node))
		return invalfc(fc, "numa_node=%d is no node with memory", sbi->numa_node);
	return get_tree_nodev(fc, mxlfs_fill_super);
}

static void mxlfs_free_sbi(struct mxlfs_sb_info *sbi)
{
	if (sbi) {
		kfree(sbi->node);
		kvfree(sbi->wanted_tables);
	}
	kfree(sbi);
}

static void mxlfs_free_fc(struct fs_context *fc)
{
	mxlfs_free_sbi(fc->s_fs_info);
}

/*
 * A remount may repeat the mount's options but change none of them: the
 * mount reads each without a lock, and the root's mode and the structures
 * for wait= are made when it is filled. Flags such as ro are the VFS's.
 */
static int mxlfs_reconfigure(struct fs_context *fc)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(fc->root->d_sb);
	struct mxlfs_sb_info *ctx = fc->s_fs_info;
	static const char *const names[] = {
		[Opt_mode] = "mode", [Opt_huge] = "huge", [Opt_numa_node] = "numa_node",
		[Opt_size] = "size", [Opt_wait] = "wait",
	};
	unsigned int changed = 0;

	if (ctx->mode != sbi->mode)
		changed |= BIT(Opt_mode);
	if (ctx->huge != sbi->huge)
		changed |= BIT(Opt_huge);
	if (ctx->numa_node != sbi->numa_node)
		changed |= BIT(Opt_numa_node);
	if (ctx->limit != sbi->limit)
		changed |= BIT(Opt_size);
	if (ctx->wait_ms != sbi->wait_ms)
		changed |= BIT(Opt_wait);
	changed &= ctx->seen;
	if (changed)
		return invalfc(fc, "%s= cannot change on remount", names[__ffs(changed)]);
	return 0;
}

static const struct fs_context_operations mxlfs_context_ops = {
	.free		= mxlfs_free_fc,
	.parse_param	= mxlfs_parse_param,
	.get_tree	= mxlfs_get_tree,
	.reconfigure	= mxlfs_reconfigure,
};

static int mxlfs_init_fs_context(struct fs_context *fc)
{
	struct mxlfs_sb_info *sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);

	if (!sbi)
		return -ENOMEM;
	sbi->node = kcalloc(nr_node_ids, sizeof(*sbi->node), GFP_KERNEL);
	if (!sbi->node) {
		kfree(sbi);
		return -ENOMEM;
	}
	sbi->mode = MXLFS_DEFAULT_MODE;
	sbi->numa_node = NUMA_NO_NODE;
	/* As tmpfs: half of RAM unless size= says otherwise. */
	sbi->limit = (u64)(totalram_pages() / 2) << PAGE_SHIFT;
	spin_lock_init(&sbi->lazy_lock);
	INIT_LIST_HEAD(&sbi->lazy);
	spin_lock_init(&sbi->wanted_lock);
	INIT_LIST_HEAD(&sbi->wanted);
	init_waitqueue_head(&sbi->wanted_wq);
	fc->s_fs_info = sbi;
	fc->ops = &mxlfs_context_ops;
	return 0;
}

static void mxlfs_kill_sb(struct super_block *sb)
{
	struct mxlfs_sb_info *sbi = sb->s_fs_info;

	mxlfs_kill_pinned_super(sb);
	while (!list_empty(&sbi->lazy)) {
		struct mxlfs_lazy *l = list_first_entry(&sbi->lazy, struct mxlfs_lazy, node);

		list_del(&l->node);
		put_pid(l->tgid);
		kfree(l);
	}
	mxlfs_free_sbi(sbi);
}

static struct file_system_type mxlfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "mxlfs",
	.init_fs_context = mxlfs_init_fs_context,
	.parameters	= mxlfs_fs_parameters,
	.kill_sb	= mxlfs_kill_sb,
};

static void mxlfs_inode_init_once(void *p)
{
	inode_init_once(&((struct mxlfs_inode *)p)->vfs_inode);
}

static int __init mxlfs_init(void)
{
	int err;

	mxlfs_inode_cachep = kmem_cache_create("mxlfs_inode_cache",
					       sizeof(struct mxlfs_inode), 0,
					       SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
					       mxlfs_inode_init_once);
	if (!mxlfs_inode_cachep)
		return -ENOMEM;
	err = register_filesystem(&mxlfs_fs_type);
	if (err)
		kmem_cache_destroy(mxlfs_inode_cachep);
	return err;
}

static void __exit mxlfs_exit(void)
{
	unregister_filesystem(&mxlfs_fs_type);
	rcu_barrier();
	kmem_cache_destroy(mxlfs_inode_cachep);
}

module_init(mxlfs_init);
module_exit(mxlfs_exit);
MODULE_DESCRIPTION("Memory filesystem for MXL domains with source-switching views");
MODULE_LICENSE("GPL");
MODULE_ALIAS_FS("mxlfs");
MODULE_VERSION("0.1.0-rc.9"); /* x-release-please-version */
