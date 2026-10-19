#pragma once
// The play-composition contract: how a title module hands the host binary its boot.
//
// app/host is title-neutral. It knows how to BE a process (main.cpp: the build identity,
// --version / --help, the "nothing to run" report) but not how to boot any particular title --
// that composition (the recompiled EE code, the per-subsystem HLE, the ServiceStartup order,
// the launcher's first-run flow) is the title module's (games/<serial>_<slug>/composition/).
//
// A title module that CAN boot defines a library target `ps2r_title_composition` (and,
// when its recompiled IOP module exists, `ps2r_title_composition_oracle` -- the same sources
// with PS2R_IOP_ORACLE=1 over ps2r_iop_models) and DEFINES TitleMain(). app/host/CMakeLists
// then links it into `nativeee` (`nativeee_oracle`) with PS2R_TITLE_COMPOSITION=1 and main()
// hands argc/argv straight to TitleMain(): the composition owns the whole command line, as a
// composition root does. With no such target, main() reports that the linked title supplies
// no play composition -- the engine, its tests and tools do not need one.
//
// This header names no title: it is the seam a second title's composition is added across,
// exactly as TitleProfile / ActiveTitleProfile() is for the retail facts.
int TitleMain(int argc, char** argv);
