# Gemlink 4.2.0

[![](https://img.shields.io/github/v/release/gemlink/gemlink)](https://github.com/gemlink/gemlink/releases) [![](https://img.shields.io/github/release-date/gemlink/gemlink)](https://github.com/gemlink/gemlink/releases) [![](https://img.shields.io/github/downloads/gemlink/gemlink/latest/total)](https://github.com/gemlink/gemlink/releases) [![](https://img.shields.io/github/downloads/gemlink/gemlink/total)](https://github.com/gemlink/gemlink/releases) [![](https://img.shields.io/discord/398513312696107008)](https://discord.gg/GghXuUnYmU)

![]()

## What is Gemlink?

Gemlink(http://gemlink.org/) is an implementation of the "Zerocash" protocol.
Based on Bitcoin's code, it intends to offer a far higher standard of privacy
through a sophisticated zero-knowledge proving scheme that preserves
confidentiality of transaction metadata. Technical details are available
in our [Protocol Specification](https://github.com/zcash/zips/raw/master/protocol/protocol.pdf).

This software is the Gemlink client. It downloads and stores the entire history
of Gemlink transactions; depending on the speed of your computer and network
connection, the synchronization process could take a day or more once the
blockchain has reached a significant size.

## Security Warnings

**Gemlink is experimental and a work-in-progress.** Use at your own risk.

## Deprecation Policy

This release is considered deprecated 16 weeks after the release day. There
is an automatic deprecation shutdown feature which will halt the node some
time after this 16 week time period. The automatic feature is based on block
height and can be explicitly disabled.

## Building

### Install dependencies

On Ubuntu/Debian-based systems:

On Ubuntu 20.04:

```
$ sudo apt-get install \
 build-essential pkg-config libc6-dev m4 g++-multilib \
 autoconf libtool ncurses-dev unzip git python3 python3-zmq \
 zlib1g-dev curl bsdmainutils automake libtinfo5
```

On Fedora-based systems:

```
$ sudo dnf install \
      git pkgconfig automake autoconf ncurses-devel python \
      python-zmq wget gtest-devel gcc gcc-c++ libtool patch curl
```

Windows:

```
sudo apt-get install \
    build-essential pkg-config libc6-dev m4 \
    autoconf libtool ncurses-dev unzip git python \
    zlib1g-dev wget bsdmainutils automake mingw-w64
```

On Mac systems:

```
brew tap discoteq/discoteq; brew install flock
brew install autoconf autogen automake
brew install gcc5
brew install binutils
brew install protobuf
brew install coreutils
brew install wget llvm
```

### Check GCC version

gcc/g++ 9 or later is required. Gemlink has been successfully built using gcc/g++ versions 9 inclusive. Use `g++ --version` to check which version you have.

On Ubuntu Trusty, if your version is too old then you can install gcc/g++ 4.9 as follows:

```
$ sudo add-apt-repository ppa:ubuntu-toolchain-r/test
$ sudo apt-get update
sudo apt install gcc-9 g++-9
```

### Fetch the software and parameter files

Fetch our repository with git and run `fetch-params.sh` like so:

```
$ ./zcutil/fetch-params.sh
```

### Build Linux/MAC

Ensure you have successfully installed all system package dependencies as described above. Then run the build, e.g.:

```
$ git clone https://github.com/gemlink/gemlink.git
$ cd gemlink/
$ ./zcutil/build.sh
```

This should compile our dependencies and build `gemlinkd`

### Build Windows

With docker:

```
docker run -ti electriccoinco/zcashd-build-ubuntu2004 bash
apt install zstd
git clone https://github.com/gemlink/gemlink.git
cd gemlink
HOST=x86_64-w64-mingw32 ./zcutil/build.sh
```

Ensure you have successfully installed all system package dependencies as described above. Then run the build, e.g.:

```
$ git clone https://github.com/gemlink/gemlink.git
$ cd gemlink/
HOST=x86_64-w64-mingw32 ./zcutil/build.sh
```
### Build ARM

```
$ git clone https://github.com/gemlink/gemlink.git
$ cd gemlink/
HOST=aarch64-linux-gnu LDFLAGS=-s ./zcutil/build.sh
```
---

### Need Help?

- See the documentation at the [refer from Zcash Wiki](https://github.com/zcash/zcash/wiki/1.0-User-Guide)
  for help and more information.
- Ask for help on the [Gemlink](https://discuss.gemlink.org/) forum or contact us via email support@gemlink.org

Participation in the Gemlink project is subject to a
[Code of Conduct](code_of_conduct.md).

## License

For license information see the file [COPYING](COPYING).
