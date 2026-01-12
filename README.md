# Gemlink 4.2.0

[![Release](https://img.shields.io/github/v/release/gemlink/gemlink)](https://github.com/gemlink/gemlink/releases)
[![Release date](https://img.shields.io/github/release-date/gemlink/gemlink)](https://github.com/gemlink/gemlink/releases)
[![Downloads (latest)](https://img.shields.io/github/downloads/gemlink/gemlink/latest/total)](https://github.com/gemlink/gemlink/releases)
[![Downloads (total)](https://img.shields.io/github/downloads/gemlink/gemlink/total)](https://github.com/gemlink/gemlink/releases)
[![Discord](https://img.shields.io/discord/398513312696107008)](https://discord.gg/GghXuUnYmU)

---

## What is Gemlink?

Gemlink (https://gemlink.org/) is an implementation of the **Zerocash** protocol.  
Based on Bitcoin’s codebase, it aims to provide a significantly higher level of
privacy through zero-knowledge proofs that preserve the confidentiality of
transaction metadata.

Technical details of the cryptographic protocol can be found in the
[Zerocash / Zcash Protocol Specification](https://github.com/zcash/zips/raw/master/protocol/protocol.pdf).

This repository contains the **Gemlink full node and wallet software**.
Running a full node requires downloading and validating the entire Gemlink
blockchain. Depending on hardware and network speed, initial synchronization
may take a significant amount of time.

---

## ⚠️ Security Warning

Gemlink is **experimental software** and under active development.

Use at your own risk.

---

## Deprecation Policy

Each Gemlink release is considered **deprecated 16 weeks after its release date**.

Gemlink includes an **automatic deprecation shutdown mechanism** based on block
height. Once a release is deprecated, the node will automatically shut down
after the deprecation threshold is reached.

This behavior can be explicitly disabled via configuration.

---

## 🔧 Building Gemlink (Linux – Recommended)

Gemlink uses a **self-contained build system (`depends/`)** to ensure
reproducible, distribution-independent builds.

This is the **officially supported and recommended build method**.

---

### ✅ Supported Operating Systems

The following systems are supported using the `depends/` build system:

- **Ubuntu 20.04 LTS**
- **Ubuntu 22.04 LTS**
- **Ubuntu 24.04 LTS**

> Older distributions (18.04 and earlier) are not supported or recommended.

The **same build procedure works unchanged** on all supported Ubuntu versions.

---

### 📦 System Dependencies (Ubuntu)

Install the minimal set of system packages required to run the build system:

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  autoconf automake libtool pkg-config \
  libssl-dev \
  libevent-dev \
  libncurses-dev \
  bsdextrautils \
  python3 \
  curl git \
  clang cmake
```
⚠️ Important

Do NOT install or use system versions of the following libraries:

Berkeley DB

Boost

ZeroMQ

All critical dependencies are built internally via the depends/ system.

🏗️ Build Instructions (Linux)

```
git clone https://github.com/gemlink/gemlink.git
cd gemlink

# 1. Build toolchain and all dependencies
cd depends
make -j$(nproc)
cd ..

# 2. Generate build system
./autogen.sh

# 3. Configure (wallet enabled – required for masternodes)
./configure --prefix="$(pwd)/depends/x86_64-pc-linux-gnu"

# 4. Build Gemlink
make -j$(nproc)
```
After a successful build, the binaries will be available in:

```
src/gemlinkd
src/gemlink-cli
src/gemlink-tx
```

🧪 Quick Verification

Verify the build and confirm that it links against depends/:

```
src/gemlinkd --version
ldd src/gemlinkd | grep depends
```

🧠 Important Notes

Gemlink must be built with wallet support if masternodes or budgeting
features are enabled.

The build uses clang + libc++ provided by depends/, not the system GCC.

The system GCC version is not relevant for supported Ubuntu builds.

The build process is identical on Ubuntu 20.04, 22.04, and 24.04.

❌ Legacy / Deprecated Build Methods

The following build methods are deprecated and not supported:

zcutil/build.sh for native Linux builds

System-wide Berkeley DB (libdb4.8)

Manual installation of Boost or ZeroMQ

GCC-only builds on modern Linux distributions

These methods may fail or produce unstable binaries.

🧪 Fedora (Experimental)

Building on Fedora is possible only via the depends/ build system.

System-provided libraries (Boost, Berkeley DB, ZeroMQ) must NOT be used.

Fedora dependencies

```
sudo dnf install -y \
  git \
  autoconf automake libtool pkg-config \
  clang llvm lld \
  openssl-devel \
  libevent-devel \
  ncurses-devel \
  python3 \
  cmake \
  curl
```

Build process

```
cd depends
make -j$(nproc)
cd ..

./autogen.sh
./configure --prefix="$(pwd)/depends/x86_64-pc-linux-gnu"
make -j$(nproc)
```
Fedora builds are considered experimental.

🔐 Cryptographic Parameters (Optional)

If your configuration requires shielded transactions or zk-SNARK parameters,
fetch them using:

```
./zcutil/fetch-params.sh
```

🌐 Cross-Compilation (Legacy / Advanced)
Windows (via Docker – legacy)

```
docker run -ti electriccoinco/zcashd-build-ubuntu2004 bash
apt install zstd
git clone https://github.com/gemlink/gemlink.git
cd gemlink
HOST=x86_64-w64-mingw32 ./zcutil/build.sh
```
ARM (legacy)

```
git clone https://github.com/gemlink/gemlink.git
cd gemlink
HOST=aarch64-linux-gnu LDFLAGS=-s ./zcutil/build.sh
```

Cross-compilation currently relies on legacy tooling and is not officially supported.

🆘 Need Help?

Refer to the Zcash Wiki

Ask questions in the Gemlink community channels

Contact support: support@gemlink.org

Participation in the Gemlink project is subject to the
Code of Conduct


License

For license information, see the file COPYING.









