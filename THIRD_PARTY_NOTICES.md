# Third-party software

The fix contains the components below; their sources are fetched at build time, pinned by commit in scripts/build-pc-compat.ps1. The same text is installed with the game as licenses\THIRD_PARTY_NOTICES.txt.

```
Third-party software in the fix
===============================

Dear ImGui (the in-game settings overlay), version 1.92.7-docking,
https://github.com/ocornut/imgui, used under the MIT License:

The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Build tools (not part of the fix): llvm-mingw (Apache-2.0 WITH LLVM-exception and the mingw-w64 licences). Optional texture tool: Real-ESRGAN ncnn-vulkan (BSD-3-Clause, models BSD-3-Clause).
