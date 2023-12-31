/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <err.h>
#include <malloc.h>
#include <stdint.h>

#include <string>

#include <benchmark/benchmark.h>

#include <unwindstack/Elf.h>
#include <unwindstack/Maps.h>
#include <unwindstack/Memory.h>
#include <unwindstack/Regs.h>

#include "Utils.h"

class ElfCreateBenchmark : public benchmark::Fixture {
 public:
  void RunBenchmark(benchmark::State& state, const std::string& elf_file) {
    MemoryTracker mem_tracker;
    for (const auto& _ : state) {
      state.PauseTiming();
      mem_tracker.StartTrackingAllocations();

      auto file_memory = unwindstack::Memory::CreateFileMemory(elf_file, 0);
      state.ResumeTiming();

      unwindstack::Elf elf(file_memory.release());
      if (!elf.Init() || !elf.valid()) {
        errx(1, "Internal Error: Cannot open elf: %s", elf_file.c_str());
      }

      state.PauseTiming();
      mem_tracker.StopTrackingAllocations();
      state.ResumeTiming();
    }
    mem_tracker.SetBenchmarkCounters(state);
  }
};

BENCHMARK_F(ElfCreateBenchmark, BM_elf_create)(benchmark::State& state) {
  RunBenchmark(state, GetElfFile());
}

BENCHMARK_F(ElfCreateBenchmark, BM_elf_create_large_compressed)(benchmark::State& state) {
  RunBenchmark(state, GetLargeCompressedFrameElfFile());
}

BENCHMARK_F(ElfCreateBenchmark, BM_elf_create_large_eh_frame)(benchmark::State& state) {
  RunBenchmark(state, GetLargeEhFrameElfFile());
}

static void InitializeBuildId(benchmark::State& state, unwindstack::Maps& maps,
                              unwindstack::MapInfo** build_id_map_info) {
  if (!maps.Parse()) {
    state.SkipWithError("Failed to parse local maps.");
    return;
  }

  // Find the libc.so share library and use that for benchmark purposes.
  *build_id_map_info = nullptr;
  for (auto& map_info : maps) {
    if (map_info->offset() == 0 && map_info->GetBuildID() != "") {
      *build_id_map_info = map_info.get();
      break;
    }
  }

  if (*build_id_map_info == nullptr) {
    state.SkipWithError("Failed to find a map with a BuildID.");
  }
}

static void BM_elf_get_build_id_from_object(benchmark::State& state) {
  unwindstack::LocalMaps maps;
  unwindstack::MapInfo* build_id_map_info;
  InitializeBuildId(state, maps, &build_id_map_info);

  unwindstack::Elf* elf = build_id_map_info->GetElf(std::shared_ptr<unwindstack::Memory>(),
                                                    unwindstack::Regs::CurrentArch());
  if (!elf->valid()) {
    state.SkipWithError("Cannot get valid elf from map.");
  }

  for (const auto& _ : state) {
    state.PauseTiming();
    unwindstack::SharedString* id = build_id_map_info->build_id();
    if (id != nullptr) {
      delete id;
      build_id_map_info->set_build_id(nullptr);
    }
    state.ResumeTiming();
    benchmark::DoNotOptimize(build_id_map_info->GetBuildID());
  }
}
BENCHMARK(BM_elf_get_build_id_from_object);

static void BM_elf_get_build_id_from_file(benchmark::State& state) {
  unwindstack::LocalMaps maps;
  unwindstack::MapInfo* build_id_map_info;
  InitializeBuildId(state, maps, &build_id_map_info);

  for (const auto& _ : state) {
    state.PauseTiming();
    unwindstack::SharedString* id = build_id_map_info->build_id();
    if (id != nullptr) {
      delete id;
      build_id_map_info->set_build_id(nullptr);
    }
    state.ResumeTiming();
    benchmark::DoNotOptimize(build_id_map_info->GetBuildID());
  }
}
BENCHMARK(BM_elf_get_build_id_from_file);
