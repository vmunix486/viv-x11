# viv-x11
vmunix's image viewer, using stb, for X11.

# Features

 - Fairly portable (no linux-specific things, _could_ probably run on BSD's)
 - Supports lots of image formats (jpeg, png, tga, bmp, psd, static gif, hdr, pic, pnm)
 - A little small (549 lines, 455 SLOC)
 - Only needs Xlib, Xaw, and Athena

# Compilation Guide

Just run `make` if you're on a distro that uses `/usr/X11/lib64`. An example would be T2 SDE. If you are running another distro, it might use something like `/lib` or `/usr/X11/lib`. You can change the path in the Makefile. Another thing you can do is also add `-flto` to the `CXXFLAGS`.

# Usage

```
viv <path to image>
```

If you start it without any arguements, then it will first try to open a file picker with Zenity, and if Zenity is not installed, then it falls back to an Athena based file inputer.

# Contributing

You can contribute by forking this repository and making your modifications. Your modifications has to be in C++. When you are wanting to merge, just explain all your modifications, and also display a picture or video of the change.
