/* l_lx_exec.c -- generic stub loader for Linux using execve()

   This file is part of the UPX executable compressor.

   Copyright (C) 1996-2000 Markus Franz Xaver Johannes Oberhumer
   Copyright (C) 1996-2000 Laszlo Molnar

   UPX and the UCL library are free software; you can redistribute them
   and/or modify them under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.
   If not, write to the Free Software Foundation, Inc.,
   59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

   Markus F.X.J. Oberhumer                   Laszlo Molnar
   markus.oberhumer@jk.uni-linz.ac.at        ml1050@cdata.tvnet.hu
 */


#if !defined(__linux__) || !defined(__i386__)
#  error "this stub must be compiled under linux/i386"
#endif

#include "linux.hh"
#include <elf.h>


/*************************************************************************
// configuration section
**************************************************************************/

// use malloc instead of the bss segement
#define USE_MALLOC


/*************************************************************************
// file util
**************************************************************************/

#undef xread
#undef xwrite

struct Extent {
    int  size;  // must be first to match size[0] uncompressed size
    char *buf;
};

static void
xread(struct Extent *const x, char *const buf, size_t const count)
{
    if (x->size < (int)count) {
        exit(127);
    }
#if 0  //{
    {
        char *p=x->buf, *q=buf;
        size_t j;
        for (j = count; 0!=j--; ++p, ++q) {
            *q = *p;
        }
    }
#else //}{
    {
        register unsigned long int __d0, __d1, __d2;
        __asm__ __volatile__( "cld; rep; movsb"
            : "=&c" (__d0), "=&D" (__d1), "=&S" (__d2)
            : "0" (count), "1" (buf), "2" (x->buf)
            : "memory");
    }
#endif  //}
    x->buf  += count;
    x->size -= count;
}

#if 1
static int xwrite(int fd, const void *buf, int count)
{
    // note: we can assert(count > 0);
    do {
        int n = write(fd, buf, count);
        if (n == -EINTR)
            continue;
        if (n <= 0)
            break;
        buf += n;               // gcc extension: add to void *
        count -= n;
    } while (count > 0);
    return count;
}
#else
#define xwrite(fd,buf,count)    ((count) - write(fd,buf,count))
#endif


/*************************************************************************
// util
**************************************************************************/

static char *upx_itoa(char *buf, unsigned long v)
{
    char *p = buf;
    {
        unsigned long k = v;
        do {
            p++;
            k /= 10;
        } while (k > 0);
    }
    buf = p;
    *p = 0;
    {
        unsigned long k = v;
        do {
            *--p = '0' + k % 10;
            k /= 10;
        } while (k > 0);
    }
    return buf;
}

static uint32_t ascii5(uint32_t r, unsigned k, char *p)
{
    do {
        unsigned char d = r % 32;
        if (d >= 26) d += '0' - 'Z' - 1;
        *--p += d;
        r /= 32;
    } while (--k > 0);
    return r;
}


#if defined(__i386__)
#  define SET2(p, c0, c1) \
        * (unsigned short *) (p) = ((c1)<<8 | (c0))
#  define SET4(p, c0, c1, c2, c3) \
        * (uint32_t *) (p) = ((c3)<<24 | (c2)<<16 | (c1)<<8 | (c0))
#  define SET3(p, c0, c1, c2) \
        SET4(p, c0, c1, c2, 0)
#else
#  define SET2(p, c0, c1) \
        (p)[0] = c0, (p)[1] = c1
#  define SET3(p, c0, c1, c2) \
        (p)[0] = c0, (p)[1] = c1, (p)[2] = c2
#  define SET4(p, c0, c1, c2, c3) \
        (p)[0] = c0, (p)[1] = c1, (p)[2] = c2, (p)[3] = c3
#endif


/*************************************************************************
// UPX & NRV stuff
**************************************************************************/

// must be the same as in p_unix.cpp !
#if !defined(USE_MALLOC)
#  define BLOCKSIZE     (512*1024)
#endif


// patch constants for our loader (le32 format)
#define UPX1            0x31585055          // "UPX1"
#define UPX2            0x32585055          // "UPX2"
#define UPX3            0x33585055          // "UPX4"
#define UPX4            0x34585055          // "UPX4"
#define UPX5            0x35585055          // "UPX5"

typedef int f_expand(
    const nrv_byte *src, nrv_uint  src_len,
          nrv_byte *dst, nrv_uint *dst_len );

/*************************************************************************
// upx_main - called by our entry code
//
// This function is optimized for size.
**************************************************************************/

void upx_main(
    char *argv[],
    char *envp[],
    Elf32_Ehdr const *const my_ehdr,
    f_expand *const f_decompress
) __asm__("upx_main");
void upx_main(
    char *argv[],
    char *envp[],
    Elf32_Ehdr const *const my_ehdr,
    f_expand *const f_decompress
)
{
#define PAGE_MASK (~0<<12)
    // file descriptors
    int fdi, fdo;
    Elf32_Phdr const *const phdr = (Elf32_Phdr const *)
        (my_ehdr->e_phoff + (char const *)my_ehdr);
    struct Extent xi = { phdr[1].p_memsz, (char *)phdr[1].p_vaddr };
    char *next_unmap = (char *)(PAGE_MASK & (unsigned)xi.buf);
    struct p_info header;

    // for getpid()
    pid_t pid;

    // temporary file name (max 14 chars)
    static char tmpname_buf[] = "/tmp/upxAAAAAAAAAAA";
    char *tmpname = tmpname_buf;
    char procself_buf[24];  // /proc/PPPPP/fd/XX
    char *procself;

    // decompression buffer
#if defined(USE_MALLOC)
    unsigned char *buf;
    static int malloc_args[6] = {
        0, UPX5, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0
    };
#else
    static unsigned char buf[BLOCKSIZE + OVERHEAD];
#endif

    //
    // ----- Step 0: set /proc/self using /proc/<pid> -----
    //

    //personality(PER_LINUX);
    pid = getpid();
    SET4(procself_buf + 0, '/', 'p', 'r', 'o');
    SET2(procself_buf + 4, 'c', '/');
    procself = upx_itoa(procself_buf + 6, pid);
    *procself++ = '/';


    //
    // ----- Step 1: prepare input file -----
    //

    // Read header.
    xread(&xi, (void *)&header, sizeof(header));

    // Paranoia. Make sure this is actually our expected executable
    // by checking the random program id. (The id is both stored
    // in the header and patched into this stub.)
    if (header.p_progid != UPX2)
        goto error1;


    //
    // ----- Step 2: prepare temporary output file -----
    //

    // Compute name of temporary output file in tmpname[].
    // Protect against Denial-of-Service attacks.
    {
        char *p = tmpname_buf + sizeof(tmpname_buf) - 1;

        // Compute the last 4 characters (20 bits) from getpid().
        uint32_t r = ascii5((uint32_t)pid, 4, p); p-=4;

        // Provide 4 random bytes from our program id.
        r ^= header.p_progid;
        // Mix in 4 runtime random bytes.
        // Don't consume precious bytes from /dev/urandom.
        {
#if 1
            struct timeval tv;
            gettimeofday(&tv, 0);
            r ^= (uint32_t) tv.tv_sec;
            r ^= ((uint32_t) tv.tv_usec) << 12;      // shift into high-bits
#else
            // using adjtimex() may cause portability problems
            static struct timex tx;
            adjtimex(&tx);
            r ^= (uint32_t) tx.time.tv_sec;
            r ^= ((uint32_t) tx.time.tv_usec) << 12; // shift into high-bits
            r ^= (uint32_t) tx.errcnt;
#endif
        }
        // Compute 7 more characters from the 32 random bits.
        ascii5(r, 7, p);
    }

    // Just in case, remove the file.
    {
        int err = unlink(tmpname);
        if (err != -ENOENT && err != 0)
            goto error1;
    }

    // Create the temporary output file.
    fdo = open(tmpname, O_WRONLY | O_CREAT | O_EXCL, 0700);
#if 0
    // Save some bytes of code - the ftruncate() below will fail anyway.
    if (fdo < 0)
        goto error;
#endif

    // Set expected file size.
    if (ftruncate(fdo, header.p_filesize) != 0)
        goto error;


    //
    // ----- Step 3: setup memory -----
    //

#if defined(USE_MALLOC)
    buf = mmap(malloc_args);
    if ((unsigned long) buf >= (unsigned long) -4095)
        goto error;
#else
    if (header.p_blocksize > BLOCKSIZE)
        goto error;
#endif


    //
    // ----- Step 4: decompress blocks -----
    //

    for (;;)
    {
        struct {
            int32_t sz_unc;  // uncompressed
            int32_t sz_cpr;  //   compressed
        } h;
        //   Note: if h.sz_unc == h.sz_cpr then the block was not
        //   compressible and is stored in its uncompressed form.
        int i;

        // Read and check block sizes.
        xread(&xi, (void *)&h, sizeof(h));
        if (h.sz_unc == 0)                       // uncompressed size 0 -> EOF
        {
            if (h.sz_cpr != UPX_MAGIC_LE32)      // h.sz_cpr must be h->magic
                goto error;
            if (header.p_filesize != 0)        // all bytes must be written
                goto error;
            break;
        }
        if (h.sz_cpr <= 0)
            goto error;
        if (h.sz_cpr > h.sz_unc || h.sz_unc > (int32_t)header.p_blocksize)
            goto error;
        // Now we have:
        //   assert(h.sz_cpr <= h.sz_unc);
        //   assert(h.sz_unc > 0 && h.sz_unc <= blocksize);
        //   assert(h.sz_cpr > 0 && h.sz_cpr <= blocksize);

        header.p_filesize -= h.sz_unc;
        if (h.sz_cpr < h.sz_unc) { // Decompress block.
            nrv_uint out_len;
            i = (*f_decompress)(xi.buf, h.sz_cpr, buf, &out_len);
            if (i != 0 || out_len != (nrv_uint)h.sz_unc)
                goto error;
            i = xwrite(fdo,    buf, h.sz_unc);
        }
        else { // Incompressible block
            i = xwrite(fdo, xi.buf, h.sz_unc);
        }
        xi.buf  += h.sz_cpr;
        xi.size -= h.sz_cpr;

        if (xi.size < 0 || i != 0) {
// error exit is here in the middle to keep the jumps short.
        error:
            (void) unlink(tmpname);
        error1:
            // Note: the kernel will close all open files and
            //       unmap any allocated memory.
            for (;;)
                (void) exit(127);
        }

        // We will never touch these pages again.
        i = (PAGE_MASK & (unsigned)xi.buf) - (unsigned)next_unmap;
        munmap(next_unmap, i);
        next_unmap += i;
    }


    //
    // ----- Step 5: release resources -----
    //

#if defined(USE_MALLOC)
    munmap(buf, malloc_args[1]);
#endif

    if (close(fdo) != 0)
        goto error;


    //
    // ----- Step 6: try to start program via /proc/self/fd/X -----
    //

    // Many thanks to Andi Kleen <ak@muc.de> and
    // Jamie Lokier <nospam@cern.ch> for this nice idea.

    // Open the temp file.
    fdi = open(tmpname, O_RDONLY, 0);
    if (fdi < 0)
        goto error;

    // Compute name of temp fdi.
    SET3(procself, 'f', 'd', '/');
    upx_itoa(procself + 3, fdi);

    // Check for working /proc/self/fd/X by accessing the
    // temp file again, now via temp fdi.
#define err fdo
    err = access(procself_buf, R_OK | X_OK);
    if (err == UPX3)
    {
        // Now it's safe to unlink the temp file (as it is still open).
        unlink(tmpname);
        // Set the file close-on-exec.
        fcntl(fdi, F_SETFD, FD_CLOEXEC);
        // Execute the original program via /proc/self/fd/X.
        execve(procself_buf, argv, envp);
        // If we get here we've lost.
    }
#undef err

    // The proc filesystem isn't working. No problem.
    close(fdi);


    //
    // ----- Step 7: start program in /tmp  -----
    //

    // Fork off a subprocess to clean up.
    // We have to do this double-fork trick to keep a zombie from
    // hanging around if the spawned original program doesn't check for
    // subprocesses (as well as to prevent the real program from getting
    // confused about this subprocess it shouldn't have).
    // Thanks to Adam Ierymenko <api@one.net> for this solution.

    if (fork() == 0)
    {
        if (fork() == 0)
        {
            // Sleep 3 seconds, then remove the temp file.
            static const struct timespec ts = { UPX4, 0 };
            nanosleep(&ts, 0);
            unlink(tmpname);
        }
        exit(0);
    }

    // Wait for the first fork()'d process to die.
    waitpid(-1, (int *)0, 0);

    // Execute the original program.
    execve(tmpname, argv, envp);


    //
    // ----- Step 8: error exit -----
    //

    // If we return from execve() there was an error. Give up.
    goto error;
}


/*
vi:ts=4:et:nowrap
*/

