# Vendored 7-Zip SDK headers

These interface headers come from the [7-Zip source mirror](https://github.com/ip7z/7zip)
(`ip7z/7zip`, branch `main`, commit `9128b80e3a471108678e2b3ec3c985861c8d7b0f`,
7-Zip 26.x era).

Only the minimal set needed to build an external archive format plugin is
vendored:

    C/7zTypes.h, C/7zWindows.h, C/Compiler.h
    CPP/Common/{Common0.h,Defs.h,MyGuidDef.h,MyTypes.h,MyUnknown.h,MyWindows.h,NewHandler.h,Types.h*}
    CPP/7zip/{IStream.h,IProgress.h,IDecl.h,PropID.h}
    CPP/7zip/Archive/IArchive.h

(*) `CPP/Common/Types.h` was replaced by `CPP/Common/MyTypes.h` depending on
what the downloaded revision provides; the set above is what compiles.

## License

7-Zip is Copyright (C) 1999-2026 Igor Pavlov. The files in this directory are
part of the 7-Zip SDK and are distributed under the GNU LGPL 2.1 or later
(see the "License.txt"/source headers in the upstream repository). They are
used here only as interface declarations for a plugin that is loaded by the
user's own 7-Zip installation at run time.

Upstream: https://www.7-zip.org / https://github.com/ip7z/7zip
