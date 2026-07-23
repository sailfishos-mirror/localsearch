#!/bin/env python3

# Copyright (C) 2026, Carlos Garnacho <carlosg@gnome.org>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
# 02110-1301, USA.

# Helper to turn within a pseudo-tty, in order to test pager
# detection and usage.

import argparse
import os
import pty
import sys
import termios
import signal

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-s', metavar='WxH', type=str)
    (args, rest) = parser.parse_known_args(sys.argv)

    width = 1000
    height = 2000

    if args.s:
        size = args.s.split('x')
        width = int(size[0])
        height = int(size[1])

    rest.pop(0)
    if not rest or rest[0] != '--':
        parser.error('Separator not found')
    else:
        rest.pop(0)

    command = rest
    [pid, fd] = pty.fork()
    if pid == 0:
        os.execlp(command[0], *command)
    else:
        termios.tcsetwinsize(fd, [height, width])
        data = b""

        # Forward termination
        def signal_handler(signal, frame):
            os.kill(pid, signal)

        signal.signal(signal.SIGINT, signal_handler)
        signal.signal(signal.SIGTERM, signal_handler)

        while True:
            try:
                buf = os.read(fd, 1024)
                data += buf
            except:
                break

        print (data.decode('utf-8'))
        ret = os.waitpid(pid, 0)
        os.close(fd)
        sys.exit(os.waitstatus_to_exitcode(ret[1]))
