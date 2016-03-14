#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);

__visible struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0xf62135ae, __VMLINUX_SYMBOL_STR(module_layout) },
	{ 0xfb1e5762, __VMLINUX_SYMBOL_STR(nvm_unregister_target) },
	{ 0x8088e1e9, __VMLINUX_SYMBOL_STR(nvm_register_target) },
	{ 0xc0cd3b13, __VMLINUX_SYMBOL_STR(___ratelimit) },
	{ 0x301bd560, __VMLINUX_SYMBOL_STR(bio_endio) },
	{ 0x1000e51, __VMLINUX_SYMBOL_STR(schedule) },
	{ 0x78d954fb, __VMLINUX_SYMBOL_STR(bio_reset) },
	{ 0xce2c45cc, __VMLINUX_SYMBOL_STR(wait_for_completion_io) },
	{ 0xf139e5cd, __VMLINUX_SYMBOL_STR(bio_add_pc_page) },
	{ 0xe3a873f6, __VMLINUX_SYMBOL_STR(bio_alloc_bioset) },
	{ 0xb8575b48, __VMLINUX_SYMBOL_STR(fs_bio_set) },
	{ 0x2d7cc380, __VMLINUX_SYMBOL_STR(nvm_put_blk) },
	{ 0xc39592c7, __VMLINUX_SYMBOL_STR(nvm_erase_blk) },
	{ 0xf11543ff, __VMLINUX_SYMBOL_STR(find_first_zero_bit) },
	{ 0x6feb6b0a, __VMLINUX_SYMBOL_STR(nvm_submit_io) },
	{ 0x3a968a9a, __VMLINUX_SYMBOL_STR(nvm_dev_dma_alloc) },
	{ 0x78764f4e, __VMLINUX_SYMBOL_STR(pv_irq_ops) },
	{ 0xe5815f8a, __VMLINUX_SYMBOL_STR(_raw_spin_lock_irq) },
	{ 0x44b1d426, __VMLINUX_SYMBOL_STR(__dynamic_pr_debug) },
	{ 0xc57a2fde, __VMLINUX_SYMBOL_STR(kmem_cache_destroy) },
	{ 0xa78079c4, __VMLINUX_SYMBOL_STR(kmem_cache_create) },
	{ 0x4b46dd50, __VMLINUX_SYMBOL_STR(blk_queue_max_hw_sectors) },
	{ 0x672681a4, __VMLINUX_SYMBOL_STR(blk_queue_logical_block_size) },
	{ 0x9580deb, __VMLINUX_SYMBOL_STR(init_timer_key) },
	{ 0x43a53735, __VMLINUX_SYMBOL_STR(__alloc_workqueue_key) },
	{ 0xd6ee688f, __VMLINUX_SYMBOL_STR(vmalloc) },
	{ 0x183fa88b, __VMLINUX_SYMBOL_STR(mempool_alloc_slab) },
	{ 0x8a99a016, __VMLINUX_SYMBOL_STR(mempool_free_slab) },
	{ 0x26cb34a2, __VMLINUX_SYMBOL_STR(mempool_create) },
	{ 0x53326531, __VMLINUX_SYMBOL_STR(mempool_alloc_pages) },
	{ 0xd985dc99, __VMLINUX_SYMBOL_STR(mempool_free_pages) },
	{ 0xdb7729e5, __VMLINUX_SYMBOL_STR(up_write) },
	{ 0xf532055d, __VMLINUX_SYMBOL_STR(down_write) },
	{ 0x40a9b349, __VMLINUX_SYMBOL_STR(vzalloc) },
	{ 0xd2b09ce5, __VMLINUX_SYMBOL_STR(__kmalloc) },
	{ 0xdeaa3cf7, __VMLINUX_SYMBOL_STR(kmem_cache_alloc_trace) },
	{ 0x641f8720, __VMLINUX_SYMBOL_STR(kmalloc_caches) },
	{ 0x16305289, __VMLINUX_SYMBOL_STR(warn_slowpath_null) },
	{ 0x6bf1c17f, __VMLINUX_SYMBOL_STR(pv_lock_ops) },
	{ 0xe259ae9e, __VMLINUX_SYMBOL_STR(_raw_spin_lock) },
	{ 0xb8c3a7, __VMLINUX_SYMBOL_STR(mempool_alloc) },
	{ 0xdb931cc6, __VMLINUX_SYMBOL_STR(nvm_dev_dma_free) },
	{ 0xfd57dbd7, __VMLINUX_SYMBOL_STR(bio_put) },
	{ 0xad6e4bb6, __VMLINUX_SYMBOL_STR(mempool_free) },
	{ 0x1916e38c, __VMLINUX_SYMBOL_STR(_raw_spin_unlock_irqrestore) },
	{ 0x680ec266, __VMLINUX_SYMBOL_STR(_raw_spin_lock_irqsave) },
	{ 0x16e5c2a, __VMLINUX_SYMBOL_STR(mod_timer) },
	{ 0x7d11c268, __VMLINUX_SYMBOL_STR(jiffies) },
	{ 0xfb578fc5, __VMLINUX_SYMBOL_STR(memset) },
	{ 0x8d795903, __VMLINUX_SYMBOL_STR(nvm_get_blk) },
	{ 0x2e0d2f7f, __VMLINUX_SYMBOL_STR(queue_work_on) },
	{ 0xb2d5a552, __VMLINUX_SYMBOL_STR(complete) },
	{ 0x42160169, __VMLINUX_SYMBOL_STR(flush_workqueue) },
	{ 0x6c09c2a4, __VMLINUX_SYMBOL_STR(del_timer) },
	{ 0x37a0cba, __VMLINUX_SYMBOL_STR(kfree) },
	{ 0x610aaa40, __VMLINUX_SYMBOL_STR(mempool_destroy) },
	{ 0x999e8297, __VMLINUX_SYMBOL_STR(vfree) },
	{ 0x8c03d20c, __VMLINUX_SYMBOL_STR(destroy_workqueue) },
	{ 0x27e1a049, __VMLINUX_SYMBOL_STR(printk) },
	{ 0xbdfb6dbb, __VMLINUX_SYMBOL_STR(__fentry__) },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=";


MODULE_INFO(srcversion, "3F6421E0A5589A588F33700");
