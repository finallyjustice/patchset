/* DATE 2018-08-30 */

/*
 * This patch is used to reproduce the xen-swiotlb bug.
 *
 * The flow in the test:
 *
 * 1. Allocate N pages on dom0 via __get_free_pages().
 *
 * 2. Re-allocate pages again if the first M (M < N) pages are machine
 *    continous while the entire N pages are not.
 *
 * 3. Exchange entire N pages (which indeed are not machine continuous)
 *    with xen hypervisor via xen_destroy_contiguous_region().
 *
 * 4. Free those N pages via free_pages().
 *
 * As only the first M pages are continuous, dom0 might panic.
 *
 * In this test, the pages exchanged by xen_destroy_contiguous_region()
 * are not previously exchanged from xen_create_contiguous_region().
 *
 * Build the module and run below bash script:
 *
 * #!/bin/sh
 *
 * while true
 * do
 *     rmmod xen-swiotlb-test
 *     insmod xen-swiotlb-test.ko
 * done
 *
 * The output of this module in vmcore is like:
 *
 * [  657.658687] (79) first 3 out of 4 pages are machine continuous: va=0xffff88002c6d4000, pa=0x2c6d4000, ma=0x7f9fd000
 * [  657.659188]   - (0) pfn = 0x2c6d4, mfn = 0x7f9fd
 * [  657.659191]   - (1) pfn = 0x2c6d5, mfn = 0x7f9fe
 * [  657.659661]   - (2) pfn = 0x2c6d6, mfn = 0x7f9ff
 * [  657.659991]   - (3) pfn = 0x2c6d7, mfn = 0x54a17   -------> not machine continuous!
 * [  657.661091] ------------[ cut here ]------------
 * [  657.661101] WARNING: CPU: 2 PID: 4748 at arch/x86/xen/multicalls.c:xxx xen_mc_flush+0xxxx/0xxxx()
 * ....
 *
 * (XEN) mm.c:xxxx:d0 Bad page 000000000007fa00: ed=ffff83007ffcb000(0), sd=ffff830000008000, caf=180000000000000, taf=0000000000000000
 * (XEN) mm.c:xxx:d0 pg_owner 0 l1e_owner 0, but real_pg_owner -1
 * (XEN) mm.c:xxxx:d0 Error getting mfn 7fa00 (pfn ffffffffffffffff) from L1 entry 800000007fa00063 for l1e_owner=0, pg_owner=0
 *
 * A panic!
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>

#include <asm/xen/page.h>
#include <xen/page.h>
#include <xen/xen-ops.h>

/*
 * To verify if the pfn are machine continuous
 *
 * If machine continuously, the mfn of each pfn should be continuous
 */
static int check_pages_physically_contiguous(unsigned long xen_pfn,
					     unsigned int offset,
					     size_t length)
{
	unsigned long next_bfn;
	int i;
	int nr_pages;

	next_bfn = pfn_to_bfn(xen_pfn);
	nr_pages = (offset + length + XEN_PAGE_SIZE-1) >> XEN_PAGE_SHIFT;

	for (i = 1; i < nr_pages; i++) {
		if (pfn_to_bfn(++xen_pfn) != ++next_bfn)
			return 0;
	}
	return 1;
}

/*
 * Convert physical address to machine address
 *
 * phys: physical address
 * bus : machine address
 */
static inline dma_addr_t xen_phys_to_bus(phys_addr_t paddr)
{
	unsigned long bfn = pfn_to_bfn(XEN_PFN_DOWN(paddr));
	dma_addr_t dma = (dma_addr_t)bfn << XEN_PAGE_SHIFT;

	dma |= paddr & ~XEN_PAGE_MASK;

	return dma;
}

/*
 * number of machine continuous pages in the test
 *
 * NUM_PAGE should never be the order of 2, in order to hit the issue
 */
#define NUM_PAGE  3
#define NUM_RETRY 10000

static int __init swiotlb_test_init(void)
{
	struct device dev;
	void *va;        // virtual address
	phys_addr_t pa;  // physical address
	dma_addr_t ma;   // machine address used in dma
	u64 dma_mask = DMA_BIT_MASK(32);
	/*
	 * We expect old_num pages  are machine continuous while new_num are not
	 */
	int old_num = NUM_PAGE;
	int old_size = old_num << PAGE_SHIFT;
	int order = get_order(old_size);
	int new_num = 1UL << order;
	int new_size = 1UL << (order + XEN_PAGE_SHIFT);
	int i, j;

	//pr_alert("start test: old_size=%d, new_size=%d, order=%d, old_num=%d, new_num=%d\n",
	//	 old_size, new_size, order, old_num, new_num);

	memset(&dev, 0, sizeof(dev));
	dev.dma_mask = &dma_mask;

	for (i = 0; i < NUM_RETRY; i++) {
		/* allocate NUM_PAGE free pages from dom0 space */
		va = (void*)__get_free_pages(GFP_KERNEL, order);

		/* add signature: once exchanged with xen, va[1] should not be 'a' */
		((char *)va)[1] = 'a';

		pa = virt_to_phys(va);     // get physical address
		ma = xen_phys_to_bus(pa);  // get machine address for dma

		/*
		 * If first new_num pages are machine continuous,
		 * exchange with xen and continue.
		 */
		if (check_pages_physically_contiguous(virt_to_pfn(va), 0, new_size)) {

			/* exchange those machine continuous pages with xen hypervisor */
			xen_destroy_contiguous_region(pa, order);
			
			if (((char *)va)[1] == 'a')
				pr_alert("perhaps failed to exchange new_num memory with xen\n");
			free_pages((unsigned long) va, order);

			continue;
		}

		/*
		 * If new_num pages are not machine continuous while the first old_num are,
		 * we might hit the issue.
		 * Exchange the pages with xen to hit the issue and enjoy!
		 */
		if (check_pages_physically_contiguous(virt_to_pfn(va), 0, old_size)) {
			
			pr_alert("(%d) first %d out of %d pages are machine continuous: va=0x%p, pa=0x%llx, ma=0x%llx\n",
				 i, old_num, new_num, va, pa, ma);
			
			for (j = 0; j < new_num; j++)
				pr_alert("  - (%d) pfn = 0x%lx, mfn = 0x%lx\n",
					 j, virt_to_pfn(va)+j, pfn_to_bfn(virt_to_pfn(va)+j));
			
			/* exchange machine non-continuous pages with xen hypervisor to hit issue */
			xen_destroy_contiguous_region(pa, order);

			/* If memory exchange works well, the signature should not exist */
			if (((char *)va)[1] == 'a')
				pr_alert("perhaps failed to exchange memory with xen\n");
			else
				pr_alert("exchanged %d pages with xen successfully\n", new_num);
			
			free_pages((unsigned long) va, order);

			goto found;
		}
		
		/* otherwise, re-try again until NUM_RETRY times */
		free_pages((unsigned long) va, order);
	}

	//pr_alert("Tried __get_free_pages() %d times and could not find %d pages that first %d are machine continuous!\n",
	//	 NUM_RETRY, new_num, old_num);

found:
	return 0;
}

static void __exit swiotlb_test_exit(void)
{
}

module_init(swiotlb_test_init);
module_exit(swiotlb_test_exit);
MODULE_LICENSE("GPL");
