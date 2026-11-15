# Third-party notices

## NVIDIA DLSS SDK / NGX SDK

The renderer (`tpwrtx_viewer.exe`) is built against the NVIDIA DLSS SDK v310.9.16 and statically links NVIDIA's NGX library
(`nvsdk_ngx_s.lib`). That object code is licensed under the **NVIDIA RTX SDKs licence**
(<https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt>), not under this repository's MIT licence. It may not be reverse
engineered, decompiled, disassembled, modified or redistributed on its own. NVIDIA's headers, libraries and DLLs
(`nvngx_dlssd.dll`, `nvngx_dlssnr.dll`) are **not** included in this repository or in its releases; users download or supply them
themselves.

This software contains source code provided by NVIDIA Corporation: the `EnvBRDFApprox2` function in `src/viewer/rt.hlsl`, from the
DLSS-RR Integration Guide (after Ray Tracing Gems, chapter 32).

This project is not sponsored or endorsed by NVIDIA. NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation.

## dxwrapper

Not included; download it from <https://github.com/elishacloud/dxwrapper>. `config/dxwrapper.ini` is a modified copy of the
sample configuration shipped with dxwrapper v1.4.7900.25.

> Copyright (C) 2025 Elisha Riedlinger
>
> This software is provided 'as-is', without any express or implied warranty. In no event will the author(s) be held liable for any
> damages arising from the use of this software. Permission is granted to anyone to use this software for any purpose, including
> commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
>
> 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use
>    this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
> 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
> 3. This notice may not be removed or altered from any source distribution.

## OptiScaler DLSS-NR module / RenoDX

The unofficial DLSS 5 support follows the approach documented by the OptiScaler DLSS-NR module
(<https://github.com/optiscaler/OptiScaler/pull/1116>): the feature number, parameter names and caller check. No code from it is
included; tpw-rtx's forwarder and blend are written independently.

## SimTheme Park / Theme Park World

© Electronic Arts / Bullfrog Productions. No game files are distributed. The game memory addresses documented in the source were
found by analysing the executable for interoperability.
