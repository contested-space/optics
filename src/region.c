/* region.c
   Rémi Attab (remi.attab@gmail.com), 25 Feb 2016
   FreeBSD-style copyright and disclaimer apply
*/


// -----------------------------------------------------------------------------
// config
// -----------------------------------------------------------------------------

static const size_t region_default_len = 1UL * 1024 * 1024;

// -----------------------------------------------------------------------------
// utils
// -----------------------------------------------------------------------------

static bool region_shm_name(const char *name, char *dest, size_t dest_len)
{
    int ret = snprintf(dest, dest_len, "optics.%s", name);
    if (ret > 0 && (size_t) ret < dest_len) return true;

    optics_fail("region name '%s' too long", name);
    return false;
}

static ssize_t region_file_len(int fd)
{
    struct stat stat;

    int ret = fstat(fd, &stat);
    if (ret == -1) {
        optics_fail_errno("unable to stat fd '%d'", fd);
        return -1;
    }

    return stat.st_size;
}


// -----------------------------------------------------------------------------
// struct
// -----------------------------------------------------------------------------

struct region_vma
{
    atomic_uintptr_t ptr;
    size_t len;

    struct region_vma *next;
};

struct region
{
    int fd;
    bool owned;
    char name[NAME_MAX];
    struct slock lock;

    size_t min_pos;
    atomic_size_t pos;
    struct region_vma vma;
};


// -----------------------------------------------------------------------------
// open/close
// -----------------------------------------------------------------------------

static bool region_unlink(const char *name)
{
    char shm_name[NAME_MAX];
    if (!region_shm_name(name, shm_name, sizeof(shm_name)))
        return false;

    if (shm_unlink(shm_name) == -1) {
        optics_fail_errno("unable to unlink region '%s'", shm_name);
        return false;
    }

    return true;
}

static bool region_create(struct region *region, const char *name, size_t len)
{
    // Wipe any leftover regions if exists.
    (void) region_unlink(name);

    memset(region, 0, sizeof(*region));
    region->owned = true;
    region->min_pos = len;

    if (!region_shm_name(name, region->name, sizeof(region->name)))
        goto fail_name;

    region->fd = shm_open(region->name, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (region->fd == -1) {
        optics_fail_errno("unable to create region '%s'", name);
        goto fail_open;
    }

    size_t vma_len = region_default_len;
    optics_assert(vma_len == align(vma_len, page_len), "misaligned vma_len: %p != %p",
            (void *) vma_len, (void *) align(vma_len, page_len));

    int ret = ftruncate(region->fd, vma_len);
    if (ret == -1) {
        optics_fail_errno("unable to resize region '%s' to '%lu'", name, vma_len);
        goto fail_truncate;
    }

    int prot = PROT_READ | PROT_WRITE;
    int flags = MAP_SHARED;
    void *vma_ptr = mmap(0, vma_len, prot, flags, region->fd, 0);
    if (vma_ptr == MAP_FAILED) {
        optics_fail_errno("unable to map region '%s' to '%lu'", name, vma_len);
        goto fail_vma;
    }

    region->vma.len = vma_len;
    atomic_init(&region->vma.ptr, (uintptr_t) vma_ptr);
    atomic_init(&region->pos, align(len, cache_line_len));

    return true;

    munmap(vma_ptr, vma_len);

  fail_vma:
  fail_truncate:
    close(region->fd);
    shm_unlink(region->name);

  fail_open:
  fail_name:
    memset(region, 0, sizeof(*region));
    return false;
}


static bool region_open(struct region *region, const char *name)
{
    memset(region, 0, sizeof(*region));
    region->owned = false;

    if (!region_shm_name(name, region->name, sizeof(region->name)))
        goto fail_name;

    region->fd = shm_open(region->name, O_RDWR, 0);
    if (region->fd == -1) {
        optics_fail_errno("unable to create region '%s'", name);
        goto fail_open;
    }


    ssize_t vma_len = region_file_len(region->fd);
    if (vma_len < 0) {
        optics_fail_errno("unable to query length of region '%s'", name);
        goto fail_len;
    }

    int prot = PROT_READ | PROT_WRITE;
    int flags = MAP_SHARED;
    void *vma_ptr = mmap(0, vma_len, prot, flags, region->fd, 0);
    if (vma_ptr== MAP_FAILED) {
        optics_fail_errno("unable to map region '%s' to '%lu'", name, vma_len);
        goto fail_vma;
    }

    region->vma.len = vma_len;
    atomic_init(&region->vma.ptr, (uintptr_t) vma_ptr);
    atomic_init(&region->pos, vma_len);

    return true;

    munmap(vma_ptr, vma_len);

  fail_vma:
  fail_len:
    close(region->fd);

  fail_open:
  fail_name:
    memset(region, 0, sizeof(*region));
    return false;
}


static bool region_close(struct region *region)
{
    optics_assert(slock_try_lock(&region->lock),
            "closing optics with active thread");

    struct region_vma *node = &region->vma;
    while (node) {
        void *vma_ptr = (void *) atomic_load(&node->ptr);
        size_t vma_len = node->len;

        if (munmap(vma_ptr, vma_len) == -1) {
            optics_fail_errno("unable to unmap region '%s': {%p, %lu}",
                    region->name, vma_ptr, vma_len);
            return false;
        }

        struct region_vma *next = node->next;
        if (node != &region->vma) free(node);
        node = next;
    }

    if (close(region->fd) == -1) {
        optics_fail_errno("unable to close region '%s'", region->name);
        return false;
    }

    if (region->owned) {
        if (shm_unlink(region->name) == -1) {
            optics_fail_errno("unable to unlink region '%s'", region->name);
            return false;
        }
    }

    return true;
}


// -----------------------------------------------------------------------------
// vma
// -----------------------------------------------------------------------------

static optics_off_t region_grow(struct region *region, size_t len)
{
    if (!region->owned) {
        optics_fail("unable to grow region '%s' in read-only mode", region->name);
        return 0;
    }

    slock_lock(&region->lock);

    // pos is only modified by grow while holding a lock. no synchronization
    // required here.
    size_t old_pos = atomic_load_explicit(&region->pos, memory_order_relaxed);
    size_t new_pos = old_pos + len;

    if (new_pos <= region->vma.len) {
        // Since we're not modifying vma.ptr, we don't need to synchronize this
        // write.
        atomic_store_explicit(&region->pos, new_pos, memory_order_relaxed);
        goto done;
    }

    size_t new_len = region->vma.len;
    while (new_len <= new_pos) new_len *= 2;

    int ret = ftruncate(region->fd, new_len);
    if (ret == -1) {
        optics_fail_errno("unable to resize region '%s' to '%lu' for len '%lu'",
                region->name, new_len, len);
        goto fail_trunc;
    }

    // We remap the entire file into memory while keeping the old mapping
    // around. This is basically a hack to keep our existing pointer valid
    // without fragmenting the memory region which would slow down calls to
    // region_ptr.

    struct region_vma *old = malloc(sizeof(*old));
    optics_assert_alloc(old);
    *old = region->vma;

    int prot = PROT_READ | PROT_WRITE;
    int flags = MAP_SHARED;
    void *ptr = mmap(0, new_len, prot, flags, region->fd, 0);
    if (ptr == MAP_FAILED) {
        optics_fail_errno("unable to map region '%s' to '%lu'", region->name, len);
        goto fail_mmap;
    }

    region->vma.next = old;
    region->vma.len = new_len;

    // pos must be written after ptr to ensure that even if we read a new ptr
    // and a stale pos then we still only have access to a correct area of
    // memory while the inverse is not true.
    atomic_store_explicit(&region->vma.ptr, (uintptr_t) ptr, memory_order_relaxed);
    atomic_store_explicit(&region->pos, new_pos, memory_order_release);

  done:
    slock_unlock(&region->lock);
    return old_pos;

  fail_mmap:
    free(old);

  fail_trunc:
    slock_unlock(&region->lock);
    return 0;
}

// Can be called concurrently with an ongoing grow operation and must therefore
// be synchronized with it.
static void * region_ptr_unsafe(struct region *region, optics_off_t off, size_t len)
{
    // pos must be read after ptr to guarantee correctness with an ongoing
    // resize. See region_grow for more details.
    void *vma_ptr = (void *) atomic_load_explicit(&region->vma.ptr, memory_order_relaxed);
    size_t max_len = atomic_load_explicit(&region->pos, memory_order_acquire);

    // the two checks must be distinct because if off is too large it could wrap
    // around and be back to ok.
    if (optics_unlikely(off > max_len || off + len > max_len)) {
        optics_fail(
                "out-of-region access in region '%s' at '%p' with len '%lu' and region_len '%p'",
                region->name, (void *) off, len, (void *) max_len);
        return NULL;
    }

    return ((uint8_t *) vma_ptr) + off;
}

static void * region_ptr(struct region *region, optics_off_t off, size_t len)
{
    if (optics_unlikely(off < region->min_pos)) {
        optics_fail("accessing header in region '%s' at '%p' with len '%lu' and min_len '%p'",
                region->name, (void *) off, len, (void *) region->min_pos);
        return NULL;
    }

    return region_ptr_unsafe(region, off, len);
}
