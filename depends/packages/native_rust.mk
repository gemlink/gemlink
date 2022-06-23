package=native_rust
$(package)_version=1.61.0
$(package)_download_path=https://static.rust-lang.org/dist
$(package)_file_name_linux=rust-$($(package)_version)-x86_64-unknown-linux-gnu.tar.gz
$(package)_sha256_hash_linux=066b324239d30787ce64142d7e04912f2e1850c07db3b2354d8654e02ff8b23a
$(package)_file_name_darwin=rust-$($(package)_version)-x86_64-apple-darwin.tar.gz
$(package)_sha256_hash_darwin=d851f1a473926a5d8f111ed08002047a5dc4ad944a5b7f8d5d2f1f266b51e66a
$(package)_file_name_freebsd=rust-$($(package)_version)-x86_64-unknown-freebsd.tar.gz
$(package)_sha256_hash_freebsd=fc025538a1f429f66e0a6b2108cfc1b5167e6742cabb573bee5841ad5929b077
$(package)_file_name_aarch64_linux=rust-$($(package)_version)-aarch64-unknown-linux-gnu.tar.gz
$(package)_sha256_hash_aarch64_linux=261cd47bc3c98c9f97b601d1ad2a7d9b33c9ea63c9a351119c2f6d4e82f5d436

# Mapping from GCC canonical hosts to Rust targets
# If a mapping is not present, we assume they are identical, unless $host_os is
# "darwin", in which case we assume x86_64-apple-darwin.
$(package)_rust_target_x86_64-pc-linux-gnu=x86_64-unknown-linux-gnu
$(package)_rust_target_x86_64-w64-mingw32=x86_64-pc-windows-gnu

# Mapping from Rust targets to SHA-256 hashes
$(package)_rust_std_sha256_hash_aarch64-linux-android=458d365f6be2764dc04185928427292c348271ffb2d653a2219603877183d564
$(package)_rust_std_sha256_hash_aarch64-unknown-linux-gnu=4c70901d1cbddec9ea99fbd62b20f454d30e1ffbb48a21169ac823b3f02a1fbc
$(package)_rust_std_sha256_hash_x86_64-apple-darwin=c1eb892ddb50ebeed288b7aa8171ad46d62362bb26b2d82d2b463dfd45606dc2
$(package)_rust_std_sha256_hash_x86_64-pc-windows-gnu=75c910899ed36a90b155e3a01c21b863000675867efc56f2b68c44edd4b7e18c
$(package)_rust_std_sha256_hash_x86_64-unknown-freebsd=1528a4bc7e3ba42da164bcc7b952dfa73048333c5b9254ce2d03db6bab6081e8

define rust_target
$(if $($(1)_rust_target_$(2)),$($(1)_rust_target_$(2)),$(if $(findstring darwin,$(3)),x86_64-apple-darwin,$(if $(findstring freebsd,$(3)),x86_64-unknown-freebsd,$(2))))
endef

ifneq ($(canonical_host),$(build))
$(package)_rust_target=$(call rust_target,$(package),$(canonical_host),$(host_os))
$(package)_exact_file_name=rust-std-$($(package)_version)-$($(package)_rust_target).tar.gz
$(package)_exact_sha256_hash=$($(package)_rust_std_sha256_hash_$($(package)_rust_target))
$(package)_build_subdir=buildos
$(package)_extra_sources=$($(package)_file_name_$(build_os))

define $(package)_fetch_cmds
$(call fetch_file,$(package),$($(package)_download_path),$($(package)_download_file),$($(package)_file_name),$($(package)_sha256_hash)) && \
$(call fetch_file,$(package),$($(package)_download_path),$($(package)_file_name_$(build_os)),$($(package)_file_name_$(build_os)),$($(package)_sha256_hash_$(build_os)))
endef

define $(package)_extract_cmds
  mkdir -p $($(package)_extract_dir) && \
  echo "$($(package)_sha256_hash)  $($(package)_source)" > $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  echo "$($(package)_sha256_hash_$(build_os))  $($(package)_source_dir)/$($(package)_file_name_$(build_os))" >> $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  $(build_SHA256SUM) -c $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  mkdir $(canonical_host) && \
  tar --strip-components=1 -xf $($(package)_source) -C $(canonical_host) && \
  mkdir buildos && \
  tar --strip-components=1 -xf $($(package)_source_dir)/$($(package)_file_name_$(build_os)) -C buildos
endef

define $(package)_stage_cmds
  bash ./install.sh --without=rust-docs --destdir=$($(package)_staging_dir) --prefix=$(build_prefix) --disable-ldconfig && \
  ../$(canonical_host)/install.sh --destdir=$($(package)_staging_dir) --prefix=$(build_prefix) --disable-ldconfig
endef
else

define $(package)_stage_cmds
  bash ./install.sh --without=rust-docs --destdir=$($(package)_staging_dir) --prefix=$(build_prefix) --disable-ldconfig
endef
endif
