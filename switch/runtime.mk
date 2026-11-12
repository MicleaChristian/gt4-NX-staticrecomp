# Included by switch/Makefile when SWITCH_EE_GENERATED points at eerecomp output.
# Builds donor runtime + EE units + CoreGS BackendDk (cpuref + deko3d) into the NRO.

# Relative paths so make prerequisites survive a checkout path with spaces
# (invoke make from switch/ via the D:\gt4sr junction when possible).
DONOR      := ../private/references/ps2-web-recomp
RUNTIME_SRC := $(DONOR)/runtime/src
RUNTIME_INC := $(DONOR)/runtime/include
# VU1 AOT overlay + generated table (Phase 2 of the Switch 60 FPS plan).
VU1_RUNTIME := ../private/switch-vu1-runtime
VU1_GEN     := ../private/switch-vu1-gen
VU_RECOMP   := ../tools/vu_recomp/src
IPU_FAST_SRC := ../private/switch-ipu-runtime/ipu.cpp
EE_DIR_REL  := $(SWITCH_EE_GENERATED)
ifeq ($(strip $(EE_DIR_REL)),)
EE_DIR_REL  := ../private/switch-ee/generated
endif
EE_DIR      := $(SWITCH_EE_GENERATED_ABS)
HW_GS      := ../native/hw_gs
NATIVE     := ../native
TOOLS_DIR  := tools

RUNTIME_OBJS := $(BUILD)/rt_memory.o $(BUILD)/rt_dispatch.o $(BUILD)/rt_system.o $(BUILD)/rt_kernel.o \
  $(BUILD)/rt_hw.o $(BUILD)/rt_cop.o $(BUILD)/rt_disc.o $(BUILD)/rt_dmac.o $(BUILD)/rt_vif.o \
  $(BUILD)/rt_vu_micro.o $(BUILD)/rt_vu_jit.o $(BUILD)/rt_vu_jit_web.o $(BUILD)/rt_ipu.o \
  $(BUILD)/rt_gs.o $(BUILD)/rt_gif.o $(BUILD)/rt_queue.o $(BUILD)/rt_raster.o \
  $(BUILD)/rt_iop_cpu.o $(BUILD)/rt_iop_kernel.o $(BUILD)/rt_iop_libs.o $(BUILD)/rt_iop_hw.o \
  $(BUILD)/rt_iop_spu2.o $(BUILD)/rt_iop_sio2.o $(BUILD)/rt_iop_io.o $(BUILD)/rt_iop_cdvd.o \
  $(BUILD)/rt_iop_sif.o $(BUILD)/rt_iop_cdvdfsv.o \
  $(BUILD)/vu_sha256.o

# CoreGS (BackendDk): dispatch/options + cpuref stack + deko3d WritebackDk GPU drawer.
COREGS_OBJS := \
  $(BUILD)/hw_gs_dispatch.o $(BUILD)/hw_gs_options.o \
  $(BUILD)/hw_gs_backend_dk.o $(BUILD)/dk_gpu.o $(BUILD)/writeback_dk.o \
  $(BUILD)/cpuref_memory.o $(BUILD)/cpuref_draw.o $(BUILD)/cpuref_renderer.o \
  $(BUILD)/hw_memory.o $(BUILD)/hw_clut.o \
  $(BUILD)/hw_gs_registers.o $(BUILD)/hw_gif_parser.o $(BUILD)/hw_transfers.o \
  $(BUILD)/hw_batch.o $(BUILD)/hw_front_end.o \
  $(BUILD)/hw_gs_pixel.o $(BUILD)/hw_gs_display.o \
  $(BUILD)/coregs_nx.o $(BUILD)/race_display.o $(BUILD)/perf_nx.o

# Switch headers included by patched donor sources, CoreGS and the app. Objects that miss one of these
# keep stale slot numbers or struct layouts, so every runtime/CoreGS/app object depends on all of them.
SWITCH_SHARED_HEADERS := include/perf_nx.hpp include/coregs_nx.hpp include/gt4_switch.hpp \
  $(HW_GS)/hw_gs_backend.hpp $(HW_GS)/backend_dk/writeback_dk.hpp $(HW_GS)/backend_dk/dk_gpu.hpp \
  $(HW_GS)/cpuref/cpuref_renderer.hpp $(NATIVE)/platform/nx/deko3d_present.hpp
$(RUNTIME_OBJS) $(COREGS_OBJS) $(addprefix $(BUILD)/,$(CPPFILES:.cpp=.o)) $(addprefix $(BUILD)/,$(NXCPP:.cpp=.o)): \
  $(SWITCH_SHARED_HEADERS)

VU1_PARTS := $(shell ls "$(VU1_GEN)"/vu1_generated_*.cpp 2>/dev/null)
VU1_OBJS  := $(BUILD)/vu1_generated_table.o $(addprefix $(BUILD)/vu1_,$(addsuffix .o,$(basename $(notdir $(VU1_PARTS)))))

# UAM → DKSH → embedded headers for WritebackDk draw shaders.
UAM ?= $(DEVKITPRO)/tools/bin/uam
EMBED_DKSH := $(TOOLS_DIR)/embed_dksh.py
DK_SHADERS := $(HW_GS)/backend_dk/shaders
$(BUILD)/gs_draw_vsh_dksh.hpp: $(DK_SHADERS)/gs_draw_vsh.glsl $(EMBED_DKSH) | $(BUILD)
	@python "$(EMBED_DKSH)" "$(UAM)" vert "$<" "$@"
$(BUILD)/gs_draw_fsh_dksh.hpp: $(DK_SHADERS)/gs_draw_fsh.glsl $(EMBED_DKSH) | $(BUILD)
	@python "$(EMBED_DKSH)" "$(UAM)" frag "$<" "$@"

EE_UNITS := $(shell ls "$(EE_DIR_REL)"/*.cpp 2>/dev/null)
EE_OBJS  := $(addprefix $(BUILD)/ee_,$(addsuffix .o,$(basename $(notdir $(EE_UNITS)))))

CXXFLAGS_RT := $(CXXFLAGS) -I"$(abspath $(RUNTIME_INC))" -I"$(abspath $(EE_DIR_REL))" -DGT4_SWITCH_RUNTIME=1 \
  -I"$(abspath $(HW_GS))" -I"$(abspath $(HW_GS)/frontend)" -I"$(abspath $(HW_GS)/memory)" -I"$(abspath $(HW_GS)/clut)" \
  -I"$(abspath $(HW_GS)/pixel)" -I"$(abspath $(HW_GS)/display)" -I"$(abspath $(HW_GS)/cpuref)" -I"$(abspath $(HW_GS)/backend_dk)" \
  -I"$(abspath $(NATIVE))" -I"$(abspath include)" -I"$(abspath $(VU_RECOMP))" -I"$(abspath $(VU1_GEN))"

$(BUILD)/rt_memory.o: $(RUNTIME_SRC)/memory.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_dispatch.o: $(RUNTIME_SRC)/dispatch.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/system_nx.cpp: $(RUNTIME_SRC)/system.cpp $(TOOLS_DIR)/patch-system-boot.py | $(BUILD)
	@python "$(TOOLS_DIR)/patch-system-boot.py" "$<" "$@"

$(BUILD)/rt_system.o: $(BUILD)/system_nx.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -I"$(RUNTIME_SRC)" -c "$<" -o "$@"
$(BUILD)/rt_kernel.o: $(RUNTIME_SRC)/kernel.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_hw.o: $(RUNTIME_SRC)/hw.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_cop.o: $(RUNTIME_SRC)/cop.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_disc.o: $(RUNTIME_SRC)/disc.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_dmac.o: $(RUNTIME_SRC)/dmac.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_vif.o: $(RUNTIME_SRC)/vif.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/vu_micro_nx.cpp: $(VU1_RUNTIME)/vu_micro.cpp $(TOOLS_DIR)/patch-vu-perf.py | $(BUILD)
	@python "$(TOOLS_DIR)/patch-vu-perf.py" "$<" "$@"
$(BUILD)/rt_vu_micro.o: $(BUILD)/vu_micro_nx.cpp include/perf_nx.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -I"$(RUNTIME_SRC)" -c "$<" -o "$@"
$(BUILD)/rt_vu_jit.o: $(RUNTIME_SRC)/vu_jit.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_vu_jit_web.o: $(RUNTIME_SRC)/vu_jit_web.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_ipu.o: $(IPU_FAST_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -I"$(abspath $(RUNTIME_SRC))" -c "$<" -o "$@"
$(BUILD)/vu_sha256.o: $(VU_RECOMP)/sha256.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/vu1_generated_table.o: $(VU1_GEN)/vu1_generated.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -Wno-unused-label -Wno-unused-variable -c "$<" -o "$@"
$(BUILD)/vu1_%.o: $(VU1_GEN)/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -Wno-unused-label -Wno-unused-variable -c "$<" -o "$@"

$(BUILD)/gif_nx.cpp: $(RUNTIME_SRC)/gs/gif.cpp $(TOOLS_DIR)/patch-gif-coregs.py | $(BUILD)
	@python "$(TOOLS_DIR)/patch-gif-coregs.py" "$<" "$@"
$(BUILD)/gs_nx.cpp: $(RUNTIME_SRC)/gs/gs.cpp $(TOOLS_DIR)/patch-gs-coregs.py | $(BUILD)
	@python "$(TOOLS_DIR)/patch-gs-coregs.py" "$<" "$@"

$(BUILD)/rt_gs.o: $(BUILD)/gs_nx.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -I"$(RUNTIME_SRC)/gs" -c "$<" -o "$@"
$(BUILD)/rt_gif.o: $(BUILD)/gif_nx.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -I"$(RUNTIME_SRC)/gs" -c "$<" -o "$@"
$(BUILD)/rt_queue.o: $(RUNTIME_SRC)/gs/queue.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_raster.o: $(RUNTIME_SRC)/gs/raster.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_iop_cpu.o: $(RUNTIME_SRC)/iop/cpu.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_iop_kernel.o: $(RUNTIME_SRC)/iop/kernel.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_iop_libs.o: $(RUNTIME_SRC)/iop/libs.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_iop_hw.o: $(RUNTIME_SRC)/iop/hw.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_iop_spu2.o: $(RUNTIME_SRC)/iop/spu2.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_iop_sio2.o: $(RUNTIME_SRC)/iop/sio2.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_iop_io.o: $(RUNTIME_SRC)/iop/io.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_iop_cdvd.o: $(RUNTIME_SRC)/iop/cdvd.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_iop_sif.o: $(RUNTIME_SRC)/iop/sif.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/rt_iop_cdvdfsv.o: $(RUNTIME_SRC)/iop/cdvdfsv.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"

$(BUILD)/ee_%.o: $(EE_DIR_REL)/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -Wno-unused-label -Wno-unused-variable -c "$<" -o "$@"

$(BUILD)/game_main_nx.o: source/game_main_nx.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"

$(BUILD)/vu_register_stub.o: source/vu_register_stub.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"

$(BUILD)/coregs_nx.o: source/coregs_nx.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"

$(BUILD)/perf_nx.o: source/perf_nx.cpp include/perf_nx.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"

$(BUILD)/race_display.o: $(NATIVE)/race_display.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"

$(BUILD)/hw_gs_dispatch.o: $(HW_GS)/hw_gs_dispatch.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/hw_gs_options.o: $(HW_GS)/hw_gs_options.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/hw_gs_backend_dk.o: $(HW_GS)/backend_dk/hw_gs_backend_dk.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/dk_gpu.o: $(HW_GS)/backend_dk/dk_gpu.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/writeback_dk.o: $(HW_GS)/backend_dk/writeback_dk.cpp $(BUILD)/gs_draw_vsh_dksh.hpp $(BUILD)/gs_draw_fsh_dksh.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -I"$(abspath $(BUILD))" -c "$<" -o "$@"
$(BUILD)/cpuref_memory.o: $(HW_GS)/cpuref/cpuref_memory.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/cpuref_draw.o: $(HW_GS)/cpuref/cpuref_draw.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/cpuref_renderer.o: $(HW_GS)/cpuref/cpuref_renderer.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/hw_memory.o: $(HW_GS)/memory/hw_memory.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/hw_clut.o: $(HW_GS)/clut/hw_clut.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/hw_gs_registers.o: $(HW_GS)/frontend/hw_gs_registers.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/hw_gif_parser.o: $(HW_GS)/frontend/hw_gif_parser.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/hw_transfers.o: $(HW_GS)/frontend/hw_transfers.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/hw_batch.o: $(HW_GS)/frontend/hw_batch.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/hw_front_end.o: $(HW_GS)/frontend/hw_front_end.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/hw_gs_pixel.o: $(HW_GS)/pixel/hw_gs_pixel.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"
$(BUILD)/hw_gs_display.o: $(HW_GS)/display/hw_gs_display.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_RT) -c "$<" -o "$@"

$(BUILD)/$(TARGET).elf: $(BUILD) $(addprefix $(BUILD)/,$(CPPFILES:.cpp=.o)) $(addprefix $(BUILD)/,$(NXCPP:.cpp=.o)) $(RUNTIME_OBJS) $(COREGS_OBJS) $(EE_OBJS) $(VU1_OBJS)
	$(CXX) $(LDFLAGS) -o "$@" \
	  $(addprefix $(BUILD)/,$(CPPFILES:.cpp=.o)) \
	  $(addprefix $(BUILD)/,$(NXCPP:.cpp=.o)) \
	  $(RUNTIME_OBJS) $(COREGS_OBJS) $(EE_OBJS) $(VU1_OBJS) $(LIBS)
