// implement fork from user space

#include <inc/lib.h>
#include <inc/string.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW 0x800

//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
    pgfault(struct UTrapframe * utf) {
    void *   addr = (void *) utf->utf_fault_va;
    uint32_t err  = utf->utf_err;
    int      r;

    // Check that the faulting access was (1) a write, and (2) to a
    // copy-on-write page.  If not, panic.
    // Hint:
    //   Use the read-only page table mappings at uvpt
    //   (see <inc/memlayout.h>).

    // LAB 4: Your code here.
    if (! (err & FEC_WR))
        panic("pgfault: not a write");
    if (! (uvpt[PGNUM(addr)] & PTE_COW))
        panic("pgfault: not a copy-on-write page");

    // Allocate a new page, map it at a temporary location (PFTEMP),
    // copy the data from the old page to the new page, then move the new
    // page to the old page's address.
    // Hint:
    //   You should make three system calls.

    // LAB 4: Your code here.
    if (sys_page_alloc(0, PFTEMP, PTE_P | PTE_U | PTE_W) < 0)
        panic("pgfault: sys_page_alloc failed");
    addr = ROUNDDOWN(addr, PGSIZE);
    memmove(PFTEMP, addr, PGSIZE);
    if (sys_page_map(0, PFTEMP, 0, addr, PTE_P | PTE_U | PTE_W) < 0)
        panic("pgfault: sys_page_map failed");
    if (sys_page_unmap(0, PFTEMP) < 0)
        panic("pgfault: sys_page_unmap failed");
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
    duppage(envid_t envid, unsigned pn) {
    int r;

    // LAB 4: Your code here.
    extern volatile pte_t uvpt[];

    void * addr = (void *) (pn * PGSIZE);

    if (uvpt[pn] & (PTE_W|PTE_COW) && !(uvpt[pn] & PTE_SHARE)) {
        if ((r = sys_page_map(0, addr, envid, addr, PTE_P | PTE_U | PTE_COW)) < 0)
            return r;
        if ((r = sys_page_map(envid, addr, 0, addr, PTE_P | PTE_U | PTE_COW)) < 0)
            return r;
    } else 
    {
        int perm = uvpt[pn] & PTE_SHARE ? uvpt[pn]&PTE_SYSCALL : PTE_P|PTE_U;
        if ((r = sys_page_map(0, addr, envid, addr, perm)) < 0)
        return r;
    }

    return 0;
}

static int
    sduppage(envid_t envid, unsigned pn) {
    int r;

    void * addr = (void *) (pn * PGSIZE);

    pte_t pte = uvpt[pn];

    uint32_t perm = pte & PTE_SYSCALL;

    if ((r = sys_page_map(0, addr, envid, addr, perm)) < 0)
        return r;
    return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
    fork(void) {
    // LAB 4: Your code here.
    extern volatile pte_t uvpt[];
    extern volatile pde_t uvpd[];

    envid_t envid;
    int     r;

    set_pgfault_handler(pgfault);

    envid = sys_exofork();
    if (envid < 0)
        panic("fork: sys_exofork failed");
    if (envid == 0) {
        thisenv = &envs[ENVX(sys_getenvid())];
        return 0;
    }

    for (uint8_t * addr = (uint8_t *) UTEXT; addr < (uint8_t *) USTACKTOP; addr += PGSIZE)
        if ((uvpd[PDX(addr)] & PTE_P) && (uvpt[PGNUM(addr)] & PTE_P))
            if (duppage(envid, PGNUM(addr)) < 0)
                panic("fork: duppage failed");

    if ((r = sys_page_alloc(envid, (void *) (UXSTACKTOP - PGSIZE), PTE_P | PTE_U | PTE_W)) < 0)
        panic("fork: sys_page_alloc failed: %e", r);

    if ((r = sys_env_set_pgfault_upcall(envid, thisenv->env_pgfault_upcall)) < 0)
        panic("fork: sys_env_set_pgfault_upcall failed: %e", r);

    if ((r = sys_env_set_status(envid, ENV_RUNNABLE)) < 0)
        panic("fork: sys_env_set_status failed: %e", r);

    return envid;
}

// Challenge!
int sfork(void) {
    extern volatile pte_t uvpt[];
    extern volatile pde_t uvpd[];

    envid_t envid;
    int     r;

    set_pgfault_handler(pgfault);

    envid = sys_exofork();

    if (envid < 0)
        panic("sfork: sys_exofork failed");
    if (envid == 0) {
        thisenv = &envs[ENVX(sys_getenvid())];
        return 0;
    }

    bool instack = true;
    for (uint8_t * addr = (uint8_t *) (USTACKTOP - PGSIZE); addr >= (uint8_t *) UTEXT; addr -= PGSIZE)
        if ((uvpd[PDX(addr)] & PTE_P) && (uvpt[PGNUM(addr)] & PTE_P))
            if (instack)
                duppage(envid, PGNUM(addr));
            else
                sduppage(envid, PGNUM(addr));
        else
            instack = false;

    if ((r = sys_page_alloc(envid, (void *) (UXSTACKTOP - PGSIZE), PTE_P | PTE_U | PTE_W)) < 0)
        panic("fork: sys_page_alloc failed: %e", r);

    if ((r = sys_env_set_pgfault_upcall(envid, thisenv->env_pgfault_upcall)) < 0)
        panic("fork: sys_env_set_pgfault_upcall failed: %e", r);

    if ((r = sys_env_set_status(envid, ENV_RUNNABLE)) < 0)
        panic("fork: sys_env_set_status failed: %e", r);
    return envid;
}
