/*
 * mxlfs - a memory filesystem for MXL domains.
 *
 * Regular files keep their data in unevictable page cache, allocated in the
 * largest folios that fit (PMD-sized where alignment allows), so ring buffers
 * map with 2 MiB entries and register with few NIC translation entries.
 * There is no writeback and no page_mkwrite, so shared writable mappings can
 * be long-term pinned for RDMA.
 *
 * A view is a file whose pages are those of one of several equally sized
 * source files. Selecting another source zaps the view's mappings; readers
 * refault onto the new source's pages. No data is copied.
 */

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/file.h>
#include <linux/huge_mm.h>
#include <linux/magic.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/uio.h>
#include <linux/falloc.h>

#include "mxlfs_uapi.h"

#ifndef CONFIG_TRANSPARENT_HUGEPAGE
#error "mxlfs needs CONFIG_TRANSPARENT_HUGEPAGE for large page cache folios"
#endif

#define MXLFS_DEFAULT_MODE 0755

struct mxlfs_sb_info {
	umode_t mode;
	u64 limit; /* 0 means unlimited */
	atomic64_t allocated;
	atomic64_t view_selects;
	atomic64_t view_faults;
};

struct mxlfs_view {
	unsigned int count;
	unsigned int selected; /* protected by the view's invalidate_lock */
	struct file *sources[MXLFS_VIEW_MAX_SOURCES];
};

struct mxlfs_inode {
	struct inode vfs_inode;
	u64 allocated;	/* bytes charged by fallocate, under i_rwsem */
	atomic_t view_users;	/* views that use this file as a source */
	struct mxlfs_view *view;
};

static struct kmem_cache *mxlfs_inode_cachep;

static inline struct mxlfs_inode *MXLFS_I(struct inode *inode)
{
	return container_of(inode, struct mxlfs_inode, vfs_inode);
}

static inline struct mxlfs_sb_info *MXLFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static const struct super_operations mxlfs_ops;
static const struct inode_operations mxlfs_dir_inode_operations;
static const struct inode_operations mxlfs_file_inode_operations;
static const struct file_operations mxlfs_file_operations;

static bool mxlfs_charge(struct mxlfs_sb_info *sbi, u64 bytes)
{
	u64 now = atomic64_add_return(bytes, &sbi->allocated);

	if (sbi->limit && now > sbi->limit) {
		atomic64_sub(bytes, &sbi->allocated);
		return false;
	}
	return true;
}

static void mxlfs_uncharge(struct mxlfs_sb_info *sbi, u64 bytes)
{
	atomic64_sub(bytes, &sbi->allocated);
}

static struct inode *mxlfs_get_inode(struct super_block *sb,
				     const struct inode *dir, umode_t mode)
{
	struct inode *inode = new_inode(sb);

	if (!inode)
		return NULL;

	inode->i_ino = get_next_ino();
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_mapping->a_ops = &ram_aops;
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_unevictable(inode->i_mapping);
	simple_inode_init_ts(inode);

	switch (mode & S_IFMT) {
	case S_IFREG:
		mapping_set_large_folios(inode->i_mapping);
		inode->i_op = &mxlfs_file_inode_operations;
		inode->i_fop = &mxlfs_file_operations;
		break;
	case S_IFDIR:
		inode->i_op = &mxlfs_dir_inode_operations;
		inode->i_fop = &simple_dir_operations;
		inc_nlink(inode);
		break;
	case S_IFLNK:
		inode->i_op = &page_symlink_inode_operations;
		inode_nohighmem(inode);
		break;
	default:
		init_special_inode(inode, mode, 0);
		break;
	}
	return inode;
}

/* Directories */

static int mxlfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
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
	d_instantiate(dentry, inode);
	dget(dentry); /* pin the dentry, as ramfs does */
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	return 0;
}

static struct dentry *mxlfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				  struct dentry *dentry, umode_t mode)
{
	int error = mxlfs_mknod(idmap, dir, dentry, mode | S_IFDIR, 0);

	if (!error)
		inc_nlink(dir);
	return ERR_PTR(error);
}

static int mxlfs_create(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
{
	return mxlfs_mknod(idmap, dir, dentry, mode | S_IFREG, 0);
}

static int mxlfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
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
	d_instantiate(dentry, inode);
	dget(dentry);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	return 0;
}

static int mxlfs_tmpfile(struct mnt_idmap *idmap, struct inode *dir,
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

static const struct inode_operations mxlfs_dir_inode_operations = {
	.create		= mxlfs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink		= simple_unlink,
	.symlink	= mxlfs_symlink,
	.mkdir		= mxlfs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= mxlfs_mknod,
	.rename		= simple_rename,
	.tmpfile	= mxlfs_tmpfile,
};

/* Regular files */

/* The largest folio order at index that is aligned and ends by end. */
static unsigned int mxlfs_fit_order(pgoff_t index, pgoff_t end)
{
	unsigned int order = min_t(unsigned int, HPAGE_PMD_ORDER,
				   MAX_PAGECACHE_ORDER);

	while (order > 0 &&
	       (!IS_ALIGNED(index, 1UL << order) || index + (1UL << order) > end))
		order--;
	return order;
}

static long mxlfs_fallocate(struct file *file, int mode, loff_t offset,
			    loff_t len)
{
	struct inode *inode = file_inode(file);
	struct address_space *mapping = inode->i_mapping;
	struct mxlfs_inode *mi = MXLFS_I(inode);
	struct mxlfs_sb_info *sbi = MXLFS_SB(inode->i_sb);
	gfp_t gfp = mapping_gfp_mask(mapping);
	loff_t new_size = offset + len;
	pgoff_t index, end;
	long ret;

	if (mode & ~FALLOC_FL_KEEP_SIZE)
		return -EOPNOTSUPP;

	inode_lock(inode);
	ret = -EPERM;
	if (mi->view)
		goto out;
	ret = inode_newsize_ok(inode, new_size);
	if (ret)
		goto out;

	index = offset >> PAGE_SHIFT;
	end = DIV_ROUND_UP(new_size, PAGE_SIZE);
	while (index < end) {
		struct folio *folio = filemap_get_folio(mapping, index);
		unsigned int order;

		if (!IS_ERR(folio)) {
			index = folio_next_index(folio);
			folio_put(folio);
			continue;
		}

		order = mxlfs_fit_order(index, end);
		for (;;) {
			u64 bytes = PAGE_SIZE << order;
			int err;

			if (!mxlfs_charge(sbi, bytes)) {
				ret = -ENOSPC;
				goto out;
			}
			folio = filemap_alloc_folio(gfp | __GFP_ZERO |
					(order ? __GFP_NORETRY | __GFP_NOWARN : 0),
					order);
			err = folio ? filemap_add_folio(mapping, folio, index, gfp)
				    : -ENOMEM;
			if (!err)
				break;
			if (folio)
				folio_put(folio);
			mxlfs_uncharge(sbi, bytes);
			if (order == 0) {
				ret = err;
				goto out;
			}
			/* No large folio, or a smaller one already overlaps. */
			order = 0;
		}
		folio_mark_uptodate(folio);
		folio_mark_dirty(folio);
		folio_unlock(folio);
		mi->allocated += folio_size(folio);
		index = folio_next_index(folio);
		folio_put(folio);
		cond_resched();
	}

	if (!(mode & FALLOC_FL_KEEP_SIZE) && new_size > i_size_read(inode))
		i_size_write(inode, new_size);
	inode_set_ctime_current(inode);
	ret = 0;
out:
	inode_unlock(inode);
	return ret;
}

static int mxlfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			 struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct mxlfs_inode *mi = MXLFS_I(inode);
	int error;

	if ((attr->ia_valid & ATTR_SIZE) && attr->ia_size != i_size_read(inode) &&
	    (mi->view || atomic_read(&mi->view_users)))
		return -EBUSY;

	error = simple_setattr(idmap, dentry, attr);
	if (error)
		return error;

	/* Charged pages beyond a shrunk size were truncated. */
	if ((attr->ia_valid & ATTR_SIZE) && mi->allocated) {
		u64 keep = round_up(attr->ia_size, PAGE_SIZE);

		if (mi->allocated > keep) {
			mxlfs_uncharge(MXLFS_SB(inode->i_sb), mi->allocated - keep);
			mi->allocated = keep;
		}
	}
	return 0;
}

static const struct inode_operations mxlfs_file_inode_operations = {
	.setattr	= mxlfs_setattr,
	.getattr	= simple_getattr,
};

/* No page_mkwrite: shared writable mappings need no dirty tracking. */
static const struct vm_operations_struct mxlfs_file_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
};

/* Views */

static struct file *mxlfs_view_source(struct mxlfs_view *view)
{
	return view->sources[view->selected];
}

/*
 * Returns the selected source's page locked, so the core maps it as it maps
 * page cache: a PMD-sized folio with one PMD, any other large folio with all
 * of its PTEs in one fault. The rmap entries land on the source's folio.
 */
static vm_fault_t mxlfs_view_fault(struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct mxlfs_view *view = MXLFS_I(inode)->view;
	struct folio *folio;
	vm_fault_t ret = VM_FAULT_LOCKED;

	if (vmf->pgoff >= DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE))
		return VM_FAULT_SIGBUS;

	filemap_invalidate_lock_shared(inode->i_mapping);
	folio = filemap_get_folio(mxlfs_view_source(view)->f_mapping, vmf->pgoff);
	if (IS_ERR(folio)) {
		/* A hole in the source; sources are expected to be allocated. */
		ret = VM_FAULT_SIGBUS;
	} else {
		/* Held until the core has mapped it; see mxlfs_view_select. */
		folio_lock(folio);
		vmf->page = folio_file_page(folio, vmf->pgoff);
		atomic64_inc(&MXLFS_SB(inode->i_sb)->view_faults);
	}
	filemap_invalidate_unlock_shared(inode->i_mapping);
	return ret;
}

static const struct vm_operations_struct mxlfs_view_vm_ops = {
	.fault		= mxlfs_view_fault,
};

static void mxlfs_view_free(struct mxlfs_view *view)
{
	unsigned int i;

	for (i = 0; i < view->count; i++) {
		atomic_dec(&MXLFS_I(file_inode(view->sources[i]))->view_users);
		fput(view->sources[i]);
	}
	kfree(view);
}

static long mxlfs_view_set_sources(struct file *file, void __user *arg)
{
	struct inode *inode = file_inode(file);
	struct mxlfs_inode *mi = MXLFS_I(inode);
	struct mxlfs_view_sources req;
	struct mxlfs_view *view;
	unsigned int i;
	long ret;

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (req.flags || !req.count || req.count > MXLFS_VIEW_MAX_SOURCES)
		return -EINVAL;

	view = kzalloc(sizeof(*view), GFP_KERNEL);
	if (!view)
		return -ENOMEM;
	for (i = 0; i < req.count; i++) {
		struct file *src = fget(req.fds[i]);
		struct inode *si;

		ret = -EBADF;
		if (!src)
			goto fail;
		si = file_inode(src);
		ret = -EINVAL;
		if (si->i_sb != inode->i_sb || !S_ISREG(si->i_mode) ||
		    si == inode || MXLFS_I(si)->view ||
		    i_size_read(si) != i_size_read(inode)) {
			fput(src);
			goto fail;
		}
		atomic_inc(&MXLFS_I(si)->view_users);
		view->sources[view->count++] = src;
	}

	inode_lock(inode);
	ret = -EBUSY;
	if (mi->view || mi->allocated || inode->i_mapping->nrpages ||
	    mapping_mapped(inode->i_mapping) || atomic_read(&mi->view_users)) {
		inode_unlock(inode);
		goto fail;
	}
	mi->view = view;
	inode_unlock(inode);
	return 0;
fail:
	mxlfs_view_free(view);
	return ret;
}

static long mxlfs_view_select(struct file *file, u32 __user *arg)
{
	struct inode *inode = file_inode(file);
	struct mxlfs_view *view = MXLFS_I(inode)->view;
	u32 index;

	if (!view)
		return -EINVAL;
	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (get_user(index, arg))
		return -EFAULT;
	if (index >= view->count)
		return -EINVAL;

	filemap_invalidate_lock(inode->i_mapping);
	if (view->selected != index) {
		struct address_space *old = mxlfs_view_source(view)->f_mapping;
		struct folio_batch fbatch;
		pgoff_t next = 0;
		unsigned int i;

		view->selected = index;
		unmap_mapping_range(inode->i_mapping, 0, 0, 0);
		/*
		 * A fault that looked up an old-source folio before the
		 * selection changed holds it locked until its entry is
		 * installed. Waiting for each lock and zapping again removes
		 * entries installed after the first zap.
		 */
		folio_batch_init(&fbatch);
		while (filemap_get_folios(old, &next, ULONG_MAX, &fbatch)) {
			for (i = 0; i < folio_batch_count(&fbatch); i++) {
				folio_lock(fbatch.folios[i]);
				folio_unlock(fbatch.folios[i]);
			}
			folio_batch_release(&fbatch);
			cond_resched();
		}
		unmap_mapping_range(inode->i_mapping, 0, 0, 0);
		atomic64_inc(&MXLFS_SB(inode->i_sb)->view_selects);
	}
	filemap_invalidate_unlock(inode->i_mapping);
	return 0;
}

static long mxlfs_stats(struct file *file, void __user *arg)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(file_inode(file)->i_sb);
	struct mxlfs_stats st = {
		.bytes_allocated = atomic64_read(&sbi->allocated),
		.bytes_limit = sbi->limit,
		.view_selects = atomic64_read(&sbi->view_selects),
		.view_faults = atomic64_read(&sbi->view_faults),
	};

	return copy_to_user(arg, &st, sizeof(st)) ? -EFAULT : 0;
}

static long mxlfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mxlfs_view *view;
	u32 selected;

	switch (cmd) {
	case MXLFS_IOC_VIEW_SET_SOURCES:
		return mxlfs_view_set_sources(file, (void __user *)arg);
	case MXLFS_IOC_VIEW_SELECT:
		return mxlfs_view_select(file, (u32 __user *)arg);
	case MXLFS_IOC_VIEW_GET_SELECTED:
		view = MXLFS_I(file_inode(file))->view;
		if (!view)
			return -EINVAL;
		selected = READ_ONCE(view->selected);
		return put_user(selected, (u32 __user *)arg);
	case MXLFS_IOC_STATS:
		return mxlfs_stats(file, (void __user *)arg);
	}
	return -ENOTTY;
}

static int mxlfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct mxlfs_inode *mi = MXLFS_I(file_inode(file));

	file_accessed(file);
	if (!mi->view) {
		vma->vm_ops = &mxlfs_file_vm_ops;
		return 0;
	}
	/* A view is read-only: writes would land in a source. */
	if ((vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_WRITE))
		return -EACCES;
	vm_flags_clear(vma, VM_MAYWRITE);
	vma->vm_ops = &mxlfs_view_vm_ops;
	return 0;
}

static ssize_t mxlfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct mxlfs_view *view = MXLFS_I(inode)->view;
	ssize_t ret;

	if (!view)
		return generic_file_read_iter(iocb, to);

	filemap_invalidate_lock_shared(inode->i_mapping);
	ret = vfs_iter_read(mxlfs_view_source(view), to, &iocb->ki_pos, 0);
	filemap_invalidate_unlock_shared(inode->i_mapping);
	return ret;
}

static ssize_t mxlfs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	if (MXLFS_I(file_inode(iocb->ki_filp))->view)
		return -EPERM;
	return generic_file_write_iter(iocb, from);
}

static unsigned long mxlfs_get_unmapped_area(struct file *file,
		unsigned long addr, unsigned long len, unsigned long pgoff,
		unsigned long flags)
{
	return thp_get_unmapped_area(file, addr, len, pgoff, flags);
}

static const struct file_operations mxlfs_file_operations = {
	.read_iter	= mxlfs_read_iter,
	.write_iter	= mxlfs_write_iter,
	.mmap		= mxlfs_mmap,
	.fsync		= noop_fsync,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.llseek		= generic_file_llseek,
	.get_unmapped_area = mxlfs_get_unmapped_area,
	.fallocate	= mxlfs_fallocate,
	.unlocked_ioctl	= mxlfs_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

/* Superblock */

static struct inode *mxlfs_alloc_inode(struct super_block *sb)
{
	struct mxlfs_inode *mi = alloc_inode_sb(sb, mxlfs_inode_cachep, GFP_KERNEL);

	if (!mi)
		return NULL;
	mi->allocated = 0;
	atomic_set(&mi->view_users, 0);
	mi->view = NULL;
	return &mi->vfs_inode;
}

static void mxlfs_free_inode(struct inode *inode)
{
	kmem_cache_free(mxlfs_inode_cachep, MXLFS_I(inode));
}

static void mxlfs_evict_inode(struct inode *inode)
{
	struct mxlfs_inode *mi = MXLFS_I(inode);

	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	if (mi->allocated)
		mxlfs_uncharge(MXLFS_SB(inode->i_sb), mi->allocated);
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
	if (sbi->limit) {
		buf->f_blocks = sbi->limit >> PAGE_SHIFT;
		buf->f_bfree = buf->f_bavail =
			buf->f_blocks > used ? buf->f_blocks - used : 0;
	}
	return 0;
}

static int mxlfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(root->d_sb);

	if (sbi->mode != MXLFS_DEFAULT_MODE)
		seq_printf(m, ",mode=%o", sbi->mode);
	if (sbi->limit)
		seq_printf(m, ",size=%llu", sbi->limit);
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

enum { Opt_mode, Opt_size };

static const struct fs_parameter_spec mxlfs_fs_parameters[] = {
	fsparam_u32oct("mode", Opt_mode),
	fsparam_string("size", Opt_size),
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

	switch (opt) {
	case Opt_mode:
		sbi->mode = result.uint_32 & S_IALLUGO;
		break;
	case Opt_size:
		sbi->limit = memparse(param->string, &rest);
		if (*rest)
			return invalfc(fc, "Bad size: %s", param->string);
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
	sb->s_d_flags		= DCACHE_DONTCACHE;
	sb->s_time_gran		= 1;

	inode = mxlfs_get_inode(sb, NULL, S_IFDIR | sbi->mode);
	sb->s_root = d_make_root(inode);
	return sb->s_root ? 0 : -ENOMEM;
}

static int mxlfs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, mxlfs_fill_super);
}

static void mxlfs_free_fc(struct fs_context *fc)
{
	kfree(fc->s_fs_info);
}

static const struct fs_context_operations mxlfs_context_ops = {
	.free		= mxlfs_free_fc,
	.parse_param	= mxlfs_parse_param,
	.get_tree	= mxlfs_get_tree,
};

static int mxlfs_init_fs_context(struct fs_context *fc)
{
	struct mxlfs_sb_info *sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);

	if (!sbi)
		return -ENOMEM;
	sbi->mode = MXLFS_DEFAULT_MODE;
	fc->s_fs_info = sbi;
	fc->ops = &mxlfs_context_ops;
	return 0;
}

static void mxlfs_kill_sb(struct super_block *sb)
{
	struct mxlfs_sb_info *sbi = sb->s_fs_info;

	kill_litter_super(sb);
	kfree(sbi);
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
