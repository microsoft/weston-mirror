/*
 * Copyright 2026 huyuliang
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "libweston/backend-rdp/rdpclip-fd.h"
#include "zunitc/zunitc.h"

ZUC_TEST(rdpclip_fd_test, accepts_fd_without_active_transfer)
{
	int pipefd[2];
	int active_fd;

	ZUC_ASSERT_EQ(0, pipe(pipefd));

	active_fd = rdp_clipboard_take_fd(-1, pipefd[1]);

	ZUC_ASSERT_EQ(pipefd[1], active_fd);
	ZUC_ASSERT_NE(-1, fcntl(active_fd, F_GETFD));

	close(active_fd);
	close(pipefd[0]);
}

ZUC_TEST(rdpclip_fd_test, preserves_active_fd_and_closes_incoming_fd)
{
	int active_pipefd[2];
	int incoming_pipefd[2];
	int active_fd;

	ZUC_ASSERT_EQ(0, pipe(active_pipefd));
	ZUC_ASSERT_EQ(0, pipe(incoming_pipefd));

	active_fd = rdp_clipboard_take_fd(active_pipefd[1],
					  incoming_pipefd[1]);

	ZUC_ASSERT_EQ(active_pipefd[1], active_fd);
	ZUC_ASSERT_NE(-1, fcntl(active_fd, F_GETFD));
	errno = 0;
	ZUC_ASSERT_EQ(-1, fcntl(incoming_pipefd[1], F_GETFD));
	ZUC_ASSERT_EQ(EBADF, errno);

	close(active_fd);
	close(active_pipefd[0]);
	close(incoming_pipefd[0]);
}
