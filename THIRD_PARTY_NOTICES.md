# Third-party notices

## Erez Strauss's lockfree_mpmc_queue

The queue algorithm in [`mpmc/mpmc.hpp`](mpmc/mpmc.hpp) is adapted from Erez
Strauss's [`lockfree_mpmc_queue`](https://github.com/erez-strauss/lockfree_mpmc_queue),
which he presented in
[*Lockfree, Atomic, Multi Producer, Multi Consumer Queue* at CppCon
2023](https://youtu.be/M3v2GfeGJYs).

This repository modifies the upstream code by narrowing the interface to a
fixed compile-time capacity, using C++23 constraints, and adding an
atomic-entry portability adapter. The adapted implementation remains subject
to the following upstream license:

> MIT License
>
> Copyright (c) 2019 Erez Strauss, erez@erezstrauss.com
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.
