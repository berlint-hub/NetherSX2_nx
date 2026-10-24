#pragma once
#include "dk_abi.h"

namespace nxdk {
inline constexpr uint32_t Device4248[]{
  0x64c290,0x655060,0x655400,0x6555a0,0x655930,0x655ac0,0x64b5d0,0x64c260,
  0x64c310,0x6156f0,0x64f4c0,0x64e8b0,0x64f550,0x64f740,0x64f7b0,0x64f900,
  0x64fa70,0x658240,0x658270,0x658280,0x64fb60,0x653330,0x653540,0x653c60,
  0x653ef0,0x6548a0,0x6543a0,0x656b60,0x655a70};
inline constexpr uint32_t Device3668[]{
  0x674240,0x67c0b0,0x67c450,0x67c5f0,0x67c980,0x67cb10,0x673560,0x674210,
  0x6742d0,0x625d40,0x6775f0,0x6769c0,0x6275b0,0x626270,0x677680,0x677880,
  0x6778f0,0x677a40,0x677bc0,0x67f780,0x67f7b0,0x67f7c0,0x67ae70,0x6263d0,
  0x627690,0x67b210,0x67b420,0x67bba0,0x67bbf0,0x67dbb0,0x67cac0,0x626260};
inline constexpr uint32_t Host4248[]{
  0x7e5460,0x7e5500,0x7e5580,0x7e55a0,0x7e55b0,0x7e55c0,0x7e5950,0x7e5960,
  0x7e5970,0x7e5a70,0x7e5ad0,0x7e5b50,0x7e5f40,0x7e5b60,0x7e5c30,0x7e5cb0,
  0x7e5cc0,0x7e5cd0,0x7e5ce0,0x7e5fa0,0x7e55d0,0x7e57b0,0x7e6140,0x7e6200,
  0x7e5870,0x7e6040,0x7e60a0,0x7e6130,0x2f4cf0,0x7e64b0,0x7e6660};
inline constexpr uint32_t Host3668[]{
  0x7da690,0x7da730,0x7da7b0,0x7da7d0,0x7da7e0,0x7da7f0,0x7dab80,0x7dab90,
  0x7daba0,0x7daca0,0x7dad00,0x7dad80,0x7db170,0x7dad90,0x7dae60,0x7daee0,
  0x7daef0,0x7daf00,0x7daf10,0x7db1d0,0x7da800,0x7da9e0,0x7db370,0x7db430,
  0x7daaa0,0x7db270,0x7db2d0,0x7db360,0x2f3e70,0x7db6e0,0x7db890};
// Profile4248 pins libemucore_tegrax1_spinrelax.so from TEST3: the NetherSX2
// v2.2n-4248 Tegra X1 core with the spin-relax variant, plus five length-
// preserving patches taken from mininxd/NetherSX2-patch:
// scale-unclamp (drops the 0.5x GS multiplier floor), Full Texture Hash Cache
// as the default in both the preferences and GSSettings::Load paths, and two
// preset-reset sites turned into NOPs so applying a performance preset stops
// wiping memory cards, achievements and control bindings.
//
// Unlike the BAKED blobs this profile used to pin, the sixteen sites in
// g_patches_4248[] are PRISTINE here -- every one still holds the instruction
// named in its .expect field -- so patch_game() is no longer a no-op: it
// detects "v2.2n-4248" at the offset detect_core_version() looks for and
// applies all of them at load time. That is the path this table was written
// for; the baked cores merely had it applied ahead of time.
//
// CI verifies this image's SHA-256 and all GSDevice/HostDisplay vtable slots
// against this profile; none of the five Tegra patch sites collides with a
// profile offset or vtable slot. Its 338 dynamic imports are the same set the
// baked core had, all resolved by source/imports.c, and it still imports AAudio
// (20 symbols, no OpenSL ES), so source/aaudio.c is the audio path.
//
// dk_install_native_backend() uses this profile for its runtime hash/vtable
// check only when the Deko3D backend is installed. The shipped Vulkan build
// does not call that function: its selected-core hash/profile validation is a
// CI check, not a Vulkan device-runtime check.
//
// Profile3668 is retained because dk_core.cpp selects between the two at
// runtime, but no 3668 core ships: the product is Vulkan-only with a single
// core. The XGKickSkip preference is 3668-only and therefore inert.
//
// Keep these comments OUTSIDE the CoreProfile{...} braces: verify_profiles() in
// tools/check_deko3d_build.py reads the vtable addresses out of the brace body
// and any hex literal in there would be parsed as an address.
inline constexpr CoreProfile Profile4248{
  "4248", "b2dd3b6bf7c7a90fdfc80456b1b373d4b2152e74057b6b68bc03b801535c5f01",
  0xb78a20,0xb7ed58,0x6156b0,0x64ff20,0xc6cb5c0,0xa39200,0xa43400,
  0x78,0x75,2,Device4248,29,Host4248,0x271ce80,0xa38080};
inline constexpr CoreProfile Profile3668{
  "3668", "c99c2fede1f3a539326d16f46185986489d2153ec2c621d89e3704baa31170cb",
  0xb686c0,0xb6e280,0x625d00,0x678090,0xc6be458,0xa28c90,0xa32e90,
  0x98,0x95,3,Device3668,32,Host3668,0x270ce50,0xa27b10};
}
