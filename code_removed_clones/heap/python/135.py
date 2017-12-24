#
# Module which supports allocation of memory from an mmap
#
# multiprocessing/heap.py
#
# Copyright (c) 2006-2008, R Oudkerk
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
# 3. Neither the name of author nor the names of any contributors may be
#    used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.
#

import bisect
import mmap
import tempfile
import os
import sys
import threading
import itertools

import _multiprocessing
from multiprocessing.util import Finalize, info
from multiprocessing.forking import assert_spawning

__all__ = ['BufferWrapper']

#
# Inheirtable class which wraps an mmap, and from which blocks can be allocated
#

if sys.platform == 'win32': INDENT

    from _multiprocessing import win32

    class Arena(object): INDENT

        _counter = itertools.count()

        def __init__(self, size): INDENT
            self.size = size
            self.name = 'pym-%d-%d' % (os.getpid(), Arena._counter.next())
            self.buffer = mmap.mmap(-1, self.size, tagname=self.name)
            assert win32.GetLastError() == 0, 'tagname already in use'
            self._state = (self.size, self.name)

DEDENT         def __getstate__(self): INDENT
            assert_spawning(self)
            return self._state

DEDENT         def __setstate__(self, state): INDENT
            self.size, self.name = self._state = state
            self.buffer = mmap.mmap(-1, self.size, tagname=self.name)
            assert win32.GetLastError() == win32.ERROR_ALREADY_EXISTS

DEDENT DEDENT DEDENT else: INDENT

    class Arena(object): INDENT

        def __init__(self, size): INDENT
            self.buffer = mmap.mmap(-1, size)
            self.size = size
            self.name = None

#
# Class allowing allocation of chunks of memory from arenas
#

DEDENT DEDENT DEDENT class Heap(object): INDENT

    _alignment = 8

    def __init__(self, size=mmap.PAGESIZE): INDENT
        self._lastpid = os.getpid()
        self._lock = threading.Lock()
        self._size = size
        self._lengths = []
        self._len_to_seq = {}
        self._start_to_block = {}
        self._stop_to_block = {}
        self._allocated_blocks = set()
        self._arenas = []

DEDENT     @staticmethod
    def _roundup(n, alignment): INDENT
        # alignment must be a power of 2
        mask = alignment - 1
        return (n + mask) & ~mask

DEDENT     def _malloc(self, size): INDENT
        # returns a large enough block -- it might be much larger
        i = bisect.bisect_left(self._lengths, size)
        if i == len(self._lengths): INDENT
            length = self._roundup(max(self._size, size), mmap.PAGESIZE)
            self._size *= 2
            info('allocating a new mmap of length %d', length)
            arena = Arena(length)
            self._arenas.append(arena)
            return (arena, 0, length)
DEDENT         else: INDENT
            length = self._lengths[i]
            seq = self._len_to_seq[length]
            block = seq.pop()
            if not seq: INDENT
                del self._len_to_seq[length], self._lengths[i]

DEDENT DEDENT         (arena, start, stop) = block
        del self._start_to_block[(arena, start)]
        del self._stop_to_block[(arena, stop)]
        return block

DEDENT     def _free(self, block): INDENT
        # free location and try to merge with neighbours
        (arena, start, stop) = block

        try: INDENT
            prev_block = self._stop_to_block[(arena, start)]
DEDENT         except KeyError: INDENT
            pass
DEDENT         else: INDENT
            start, _ = self._absorb(prev_block)

DEDENT         try: INDENT
            next_block = self._start_to_block[(arena, stop)]
DEDENT         except KeyError: INDENT
            pass
DEDENT         else: INDENT
            _, stop = self._absorb(next_block)

DEDENT         block = (arena, start, stop)
        length = stop - start

        try: INDENT
            self._len_to_seq[length].append(block)
DEDENT         except KeyError: INDENT
            self._len_to_seq[length] = [block]
            bisect.insort(self._lengths, length)

DEDENT         self._start_to_block[(arena, start)] = block
        self._stop_to_block[(arena, stop)] = block

DEDENT     def _absorb(self, block): INDENT
        # deregister this block so it can be merged with a neighbour
        (arena, start, stop) = block
        del self._start_to_block[(arena, start)]
        del self._stop_to_block[(arena, stop)]

        length = stop - start
        seq = self._len_to_seq[length]
        seq.remove(block)
        if not seq: INDENT
            del self._len_to_seq[length]
            self._lengths.remove(length)

DEDENT         return start, stop

DEDENT     def free(self, block): INDENT
        # free a block returned by malloc()
        assert os.getpid() == self._lastpid
        self._lock.acquire()
        try: INDENT
            self._allocated_blocks.remove(block)
            self._free(block)
DEDENT         finally: INDENT
            self._lock.release()

DEDENT DEDENT     def malloc(self, size): INDENT
        # return a block of right size (possibly rounded up)
        assert 0 <= size < sys.maxint
        if os.getpid() != self._lastpid: INDENT
            self.__init__()                     # reinitialize after fork
DEDENT         self._lock.acquire()
        try: INDENT
            size = self._roundup(max(size,1), self._alignment)
            (arena, start, stop) = self._malloc(size)
            new_stop = start + size
            if new_stop < stop: INDENT
                self._free((arena, new_stop, stop))
DEDENT             block = (arena, start, new_stop)
            self._allocated_blocks.add(block)
            return block
DEDENT         finally: INDENT
            self._lock.release()

#
# Class representing a chunk of an mmap -- can be inherited
#

DEDENT DEDENT DEDENT class BufferWrapper(object): INDENT

    _heap = Heap()

    def __init__(self, size): INDENT
        assert 0 <= size < sys.maxint
        block = BufferWrapper._heap.malloc(size)
        self._state = (block, size)
        Finalize(self, BufferWrapper._heap.free, args=(block,))

DEDENT     def get_address(self): INDENT
        (arena, start, stop), size = self._state
        address, length = _multiprocessing.address_of_buffer(arena.buffer)
        assert size <= length
        return address + start

DEDENT     def get_size(self): INDENT
        return self._state[1]
DEDENT DEDENT 