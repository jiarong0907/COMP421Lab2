#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <comp421/loadinfo.h>
#include <comp421/yalnix.h>
#include <comp421/hardware.h>

typedef struct pcb {
	SavedContext *ctx;
	int two_times_pfn_of_pt0; //since the page table may be located from the middle of a page 
	int pid;
	int uid;
	char state; //RUNNING is 0, READY is 1
	int duration;
	struct pcb *next;
} pcb;

typedef void (*trap_handler)(ExceptionStackFrame *frame);

void trap_kernel_handler(ExceptionStackFrame *frame){}
void trap_clock_handler(ExceptionStackFrame *frame){}
void trap_illegal_handler(ExceptionStackFrame *frame){}
void trap_memory_handler(ExceptionStackFrame *frame){}
void trap_math_handler(ExceptionStackFrame *frame){}
void trap_tty_receive_handler(ExceptionStackFrame *frame){}
void trap_tty_transmit_handler(ExceptionStackFrame *frame){}

void *k_brk = 0;
void *p_limit = 0;
char v_enabled = 0;
int free_pf_head = -1;
int free_pf_tail = -1;
int free_pfn = 0;
struct pte *region0, *region1;
long time = 0;

extern void KernelStart(ExceptionStackFrame * frame, 
	unsigned int pmem_size, void *orig_brk, char **cmd_args) {
	k_brk = orig_brk;
	p_limit = (void *)((long)(PMEM_BASE + pmem_size));
	//initialize interrupt vector table
	trap_handler *interrupt_vector_table = calloc(TRAP_VECTOR_SIZE, sizeof(trap_handler));
	if (interrupt_vector_table == NULL) return;
	interrupt_vector_table[TRAP_KERNEL] = trap_kernel_handler;
	interrupt_vector_table[TRAP_CLOCK] = trap_clock_handler;
	interrupt_vector_table[TRAP_ILLEGAL] = trap_illegal_handler;
	interrupt_vector_table[TRAP_MEMORY] = trap_memory_handler;
	interrupt_vector_table[TRAP_MATH] = trap_math_handler;
	interrupt_vector_table[TRAP_TTY_RECEIVE] = trap_tty_receive_handler;
	interrupt_vector_table[TRAP_TTY_TRANSMIT] = trap_tty_transmit_handler;
	WriteRegister(REG_VECTOR_BASE, (RCS421RegVal)interrupt_vector_table);

	//initialize page table
	region0 = calloc(PAGE_TABLE_LEN, sizeof(struct pte));
	if (region0 == NULL) return;
	region1 = calloc(PAGE_TABLE_LEN, sizeof(struct pte));
	if (region1 == NULL) return;
	int k_base_pfn = VMEM_1_BASE >> PAGESHIFT;
	int i;
	for (i = k_base_pfn; i < UP_TO_PAGE(k_brk) >> PAGESHIFT; i++) {
		region1[i - k_base_pfn].pfn = i;
		region1[i - k_base_pfn].uprot = 0;
		region1[i - k_base_pfn].kprot = (i<(UP_TO_PAGE(&_etext) >> PAGESHIFT)?PROT_READ | PROT_EXEC:PROT_READ | PROT_WRITE);
		region1[i - k_base_pfn].valid = 1;
	}
	WriteRegister(REG_PTR0, (RCS421RegVal)region0);
	WriteRegister(REG_PTR1, (RCS421RegVal)region1);

	//initialize free physical page list
	//TODO: check if free space doesn't make up for even one single page
	free_pf_head = UP_TO_PAGE(k_brk) >> PAGESHIFT;
	for (i = free_pf_head; i < (DOWN_TO_PAGE(p_limit) >> PAGESHIFT) - 1; i++) {
		*(int *)((long)i << PAGESHIFT) = i + 1;
		free_pfn++;
	}
	*(int *)((long)i << PAGESHIFT) = MEM_INVALID_PAGES + 1;
	free_pfn++;
	for (i = MEM_INVALID_PAGES + 1; i < (DOWN_TO_PAGE(KERNEL_STACK_BASE) >> PAGESHIFT) - 1; i++) {
		*(int *)((long)i << PAGESHIFT) = i + 1;
		free_pfn++;
	}
	*(int *)((long)i << PAGESHIFT) = -1;
	free_pf_tail = i;
	free_pfn++;

	//enable VM
	WriteRegister(REG_VM_ENABLE, 1);

	//TODO: idle


}

extern int SetKernelBrk(void *addr) {
	if (!v_enabled) {
		if ((long)addr >= VMEM_1_BASE && (long)addr <= VMEM_1_LIMIT) {
			k_brk = addr;
			return 0;
		}
		else {
			perror("Brk out of bound.");
			return -1;
		}
	}
	return 0;
}

static void WriteToPhysPFN(int pfn, int value) {

	int tmp = UP_TO_PAGE(k_brk) >> PAGESHIFT;
	region1[tmp].valid = 1;
	region1[tmp].pfn = value;
	region1[tmp].kprot = PROT_READ | PROT_WRITE;
	*(int *)(long)(tmp << PAGESHIFT) = value;
	region1[tmp].valid = 0;
}

static void free_page_enq(struct pte *region, int vpn) {
	if (free_pf_head == -1) {
        free_pf_head = free_pf_tail = region[vpn].pfn;
    }
    else {
        int new_pnf = region[vpn].pfn;
        region[vpn].pfn = free_pf_tail;
        *(int *)((long)vpn << PAGESHIFT) = new_pnf;
        free_pf_tail = new_pnf;
    }
}

/*
 *  Load a program into the current process's address space.  The
 *  program comes from the Unix file identified by "name", and its
 *  arguments come from the array at "args", which is in standard
 *  argv format.
 *
 *  Returns:
 *      0 on success
 *     -1 on any error for which the current process is still runnable
 *     -2 on any error for which the current process is no longer runnable
 *
 *  This function, after a series of initial checks, deletes the
 *  contents of Region 0, thus making the current process no longer
 *  runnable.  Before this point, it is possible to return ERROR
 *  to an Exec() call that has called LoadProgram, and this function
 *  returns -1 for errors up to this point.  After this point, the
 *  contents of Region 0 no longer exist, so the calling user process
 *  is no longer runnable, and this function returns -2 for errors
 *  in this case.
 */
int
LoadProgram(char *name, char **args, ExceptionStackFrame* frame) {
    int fd;
    int status;
    struct loadinfo li;
    char *cp;
    char *cp2;
    char **cpp;
    char *argbuf;
    int i;
    unsigned long argcount;
    int size;
    int text_npg;
    int data_bss_npg;
    int stack_npg;

    TracePrintf(0, "LoadProgram '%s', args %p\n", name, args);

    if ((fd = open(name, O_RDONLY)) < 0) {
        TracePrintf(0, "LoadProgram: can't open file '%s'\n", name);
        return (-1);
    }

    status = LoadInfo(fd, &li);
    TracePrintf(0, "LoadProgram: LoadInfo status %d\n", status);
    switch (status) {
        case LI_SUCCESS:
            break;
        case LI_FORMAT_ERROR:
            TracePrintf(0,
                "LoadProgram: '%s' not in Yalnix format\n", name);
            close(fd);
            return (-1);
        case LI_OTHER_ERROR:
            TracePrintf(0, "LoadProgram: '%s' other error\n", name);
            close(fd);
            return (-1);
        default:
            TracePrintf(0, "LoadProgram: '%s' unknown error\n", name);
            close(fd);
            return (-1);
    }
    TracePrintf(0, "text_size 0x%lx, data_size 0x%lx, bss_size 0x%lx\n",
        li.text_size, li.data_size, li.bss_size);
    TracePrintf(0, "entry 0x%lx\n", li.entry);

    /*
     *  Figure out how many bytes are needed to hold the arguments on
     *  the new stack that we are building.  Also count the number of
     *  arguments, to become the argc that the new "main" gets called with.
     */
    size = 0;
    for (i = 0; args[i] != NULL; i++) {
        size += strlen(args[i]) + 1;
    }
    argcount = i;
    TracePrintf(0, "LoadProgram: size %d, argcount %d\n", size, argcount);

    /*
     *  Now save the arguments in a separate buffer in Region 1, since
     *  we are about to delete all of Region 0.
     */
    cp = argbuf = (char *)malloc(size);
    for (i = 0; args[i] != NULL; i++) {
        strcpy(cp, args[i]);
        cp += strlen(cp) + 1;
    }

    /*
     *  The arguments will get copied starting at "cp" as set below,
     *  and the argv pointers to the arguments (and the argc value)
     *  will get built starting at "cpp" as set below.  The value for
     *  "cpp" is computed by subtracting off space for the number of
     *  arguments plus 4 (for the argc value, a 0 (AT_NULL) to
     *  terminate the auxiliary vector, a NULL pointer terminating
     *  the argv pointers, and a NULL pointer terminating the envp
     *  pointers) times the size of each (sizeof(void *)).  The
     *  value must also be aligned down to a multiple of 8 boundary.
     */
    cp = ((char *)USER_STACK_LIMIT) - size;
    cpp = (char **)((unsigned long)cp & (-1 << 4));	/* align cpp */
    cpp = (char **)((unsigned long)cpp - ((argcount + 4) * sizeof(void *)));

    text_npg = li.text_size >> PAGESHIFT;
    data_bss_npg = UP_TO_PAGE(li.data_size + li.bss_size) >> PAGESHIFT;
    stack_npg = (USER_STACK_LIMIT - DOWN_TO_PAGE(cpp)) >> PAGESHIFT;

    TracePrintf(0, "LoadProgram: text_npg %d, data_bss_npg %d, stack_npg %d\n",
       text_npg, data_bss_npg, stack_npg);

    /*
     *  Make sure we will leave at least one page between heap and stack
     */
    if (MEM_INVALID_PAGES + text_npg + data_bss_npg + stack_npg +
        1 + KERNEL_STACK_PAGES >= PAGE_TABLE_LEN) {
        TracePrintf(0, "LoadProgram: program '%s' size too large for VM\n",
           name);
        free(argbuf);
        close(fd);
        return (-1);
    }

    /*
     *  And make sure there will be enough physical memory to
     *  load the new program.
     */
    // >>>> The new program will require text_npg pages of text,
    // >>>> data_bss_npg pages of data/bss, and stack_npg pages of
    // >>>> stack.  In checking that there is enough free physical
    // >>>> memory for this, be sure to allow for the physical memory
    // >>>> pages already allocated to this process that will be
    // >>>> freed below before we allocate the needed pages for
    // >>>> the new program being loaded.
    if (text_npg + data_bss_npg + stack_npg > free_pfn) {
        TracePrintf(0,
            "LoadProgram: program '%s' size too large for physical memory\n",
            name);
        free(argbuf);
        close(fd);
        return (-1);
    }

    // >>>> Initialize sp for the current process to (char *)cpp.
    // >>>> The value of cpp was initialized above.
    frame->sp = (char *)cpp;

    /*
     *  Free all the old physical memory belonging to this process,
     *  but be sure to leave the kernel stack for this process (which
     *  is also in Region 0) alone.
     */
    // >>>> Loop over all PTEs for the current process's Region 0,
    // >>>> except for those corresponding to the kernel stack (between
    // >>>> address KERNEL_STACK_BASE and KERNEL_STACK_LIMIT).  For
    // >>>> any of these PTEs that are valid, free the physical memory
    // >>>> memory page indicated by that PTE's pfn field.  Set all
    // >>>> of these PTEs to be no longer valid.
    for (i = MEM_INVALID_PAGES; i < KERNEL_STACK_BASE >> PAGESHIFT; i++) {
        if (region0[i].valid) {
            free_page_enq(region0, i);
            region0[i].valid = 0;
        }
    }

    if (free_pf_head != -1) {
    	WriteToPhysPFN(free_pf_tail, -1);
    }


    /*
     *  Fill in the page table with the right number of text,
     *  data+bss, and stack pages.  We set all the text pages
     *  here to be read/write, just like the data+bss and
     *  stack pages, so that we can read the text into them
     *  from the file.  We then change them read/execute.
     */

    // >>>> Leave the first MEM_INVALID_PAGES number of PTEs in the
    // >>>> Region 0 page table unused (and thus invalid)
     for (i = 0; i < MEM_INVALID_PAGES; i++) {
        region0[i].valid = 0;
    }

    /* First, the text pages */
    // >>>> For the next text_npg number of PTEs in the Region 0
    // >>>> page table, initialize each PTE:
    // >>>>     valid = 1
    // >>>>     kprot = PROT_READ | PROT_WRITE
    // >>>>     uprot = PROT_READ | PROT_EXEC
    // >>>>     pfn   = a new page of physical memory
    for (i = MEM_INVALID_PAGES; i < MEM_INVALID_PAGES + text_npg; i++) {
        if (free_pf_head == -1) {
            free(argbuf);
            close(fd);
            return (-2);
        }
        region0[i].valid = 1;
        region0[i].kprot = PROT_READ | PROT_WRITE;
        region0[i].uprot = PROT_READ | PROT_EXEC;
        region0[i].pfn = free_pf_head;

        free_pf_head = *(int *)((long)(i << PAGESHIFT));
    }


    /* Then the data and bss pages */
    // >>>> For the next data_bss_npg number of PTEs in the Region 0
    // >>>> page table, initialize each PTE:
    // >>>>     valid = 1
    // >>>>     kprot = PROT_READ | PROT_WRITE
    // >>>>     uprot = PROT_READ | PROT_WRITE
    // >>>>     pfn   = a new page of physical memory

    for (i = MEM_INVALID_PAGES + text_npg; i < MEM_INVALID_PAGES + text_npg + data_bss_npg; i++) {
        if (free_pf_head == -1) {
            free(argbuf);
            close(fd);
            return (-2);
        }
        region0[i].valid = 1;
        region0[i].kprot = PROT_READ | PROT_WRITE;
        region0[i].uprot = PROT_READ | PROT_WRITE;
        region0[i].pfn = free_pf_head;

        free_pf_head = *(int *)((long)(i << PAGESHIFT));
    }

    /* And finally the user stack pages */
    // >>>> For stack_npg number of PTEs in the Region 0 page table
    // >>>> corresponding to the user stack (the last page of the
    // >>>> user stack *ends* at virtual address USER_STACK_LMIT),
    // >>>> initialize each PTE:
    // >>>>     valid = 1
    // >>>>     kprot = PROT_READ | PROT_WRITE
    // >>>>     uprot = PROT_READ | PROT_WRITE
    // >>>>     pfn   = a new page of physical memory

    for (i = 0; i < stack_npg; i++) {
        if (free_pf_head == -1) {
            free(argbuf);
            close(fd);
            return (-2);
        }
        int index = (USER_STACK_LIMIT >> PAGESHIFT) - 1 - i;
        region0[index].valid = 1;
        region0[index].kprot = PROT_READ | PROT_WRITE;
        region0[index].uprot = PROT_READ | PROT_WRITE;
        region0[index].pfn = free_pf_head;

        free_pf_head = *(int *)((long)(index << PAGESHIFT));
    }

    /*
     *  All pages for the new address space are now in place.  Flush
     *  the TLB to get rid of all the old PTEs from this process, so
     *  we'll be able to do the read() into the new pages below.
     */
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_0);

    /*
     *  Read the text and data from the file into memory.
     */
    if (read(fd, (void *)MEM_INVALID_SIZE, li.text_size+li.data_size)
        != li.text_size+li.data_size) {
        TracePrintf(0, "LoadProgram: couldn't read for '%s'\n", name);
        free(argbuf);
        close(fd);
	// >>>> Since we are returning -2 here, this should mean to
	// >>>> the rest of the kernel that the current process should
	// >>>> be terminated with an exit status of ERROR reported
	// >>>> to its parent process.
        return (-2);
    }

    close(fd);			/* we've read it all now */

    /*
     *  Now set the page table entries for the program text to be readable
     *  and executable, but not writable.
     */
    // >>>> For text_npg number of PTEs corresponding to the user text
    // >>>> pages, set each PTE's kprot to PROT_READ | PROT_EXEC.
    for (i = MEM_INVALID_PAGES; i < MEM_INVALID_PAGES + text_npg; i++) {
        region0[i].kprot = PROT_READ | PROT_EXEC;
    }

    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_0);

    /*
     *  Zero out the bss
     */
    memset((void *)(MEM_INVALID_SIZE + li.text_size + li.data_size),
         '\0', li.bss_size);

    /*
     *  Set the entry point in the exception frame.
     */
    //>>>> Initialize pc for the current process to (void *)li.entry
    frame->pc = (void *)li.entry;

    /*
     *  Now, finally, build the argument list on the new stack.
     */
    *cpp++ = (char *)argcount;		/* the first value at cpp is argc */
    cp2 = argbuf;
    for (i = 0; i < argcount; i++) {      /* copy each argument and set argv */
        *cpp++ = cp;
        strcpy(cp, cp2);
        cp += strlen(cp) + 1;
        cp2 += strlen(cp2) + 1;
    }
    free(argbuf);
    *cpp++ = NULL;	/* the last argv is a NULL pointer */
    *cpp++ = NULL;	/* a NULL pointer for an empty envp */
    *cpp++ = 0;		/* and terminate the auxiliary vector */

    /*
     *  Initialize all regs[] registers for the current process to 0,
     *  initialize the PSR for the current process also to 0.  This
     *  value for the PSR will make the process run in user mode,
     *  since this PSR value of 0 does not have the PSR_MODE bit set.
     */
    // >>>> Initialize regs[0] through regs[NUM_REGS-1] for the
    // >>>> current process to 0.
    // >>>> Initialize psr for the current process to 0.
    for (i = 0; i < NUM_REGS; i++) {
        frame->regs[i] = 0;
    }
    frame->psr = 0;
    return (0);
}