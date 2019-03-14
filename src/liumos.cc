#include "liumos.h"
#include "cpu_context.h"
#include "execution_context.h"
#include "hpet.h"

LiumOS* liumos;
EFI::MemoryMap efi_memory_map;
PhysicalPageAllocator* dram_allocator;
PhysicalPageAllocator* pmem_allocator;
LocalAPIC bsp_local_apic;
CPUFeatureSet cpu_features;
SerialPort com1;
File hello_bin_file;
File liumos_elf_file;
HPET hpet;

LiumOS liumos_;
PhysicalPageAllocator dram_allocator_;
PhysicalPageAllocator pmem_allocator_;
Console main_console_;
KeyboardController keyboard_ctrl_;

File logo_file;

void FreePages(PhysicalPageAllocator* allocator,
               void* phys_addr,
               uint64_t num_of_pages) {
  const uint32_t prox_domain =
      liumos->acpi.srat ? liumos->acpi.srat->GetProximityDomainForAddrRange(
                              reinterpret_cast<uint64_t>(phys_addr),
                              num_of_pages << kPageSizeExponent)
                        : 0;
  allocator->FreePagesWithProximityDomain(phys_addr, num_of_pages, prox_domain);
}

void InitDRAMManagement(EFI::MemoryMap& map) {
  dram_allocator = &dram_allocator_;
  new (dram_allocator) PhysicalPageAllocator();
  int available_pages = 0;
  for (int i = 0; i < map.GetNumberOfEntries(); i++) {
    const EFI::MemoryDescriptor* desc = map.GetDescriptor(i);
    if (desc->type != EFI::MemoryType::kConventionalMemory)
      continue;
    available_pages += desc->number_of_pages;
    FreePages(dram_allocator, reinterpret_cast<void*>(desc->physical_start),
              desc->number_of_pages);
  }
  PutStringAndHex("Available DRAM (KiB)", available_pages * 4);
}

void InitPMEMManagement() {
  using namespace ACPI;
  pmem_allocator = &pmem_allocator_;
  new (pmem_allocator) PhysicalPageAllocator();
  if (!liumos->acpi.nfit) {
    PutString("NFIT not found. There are no PMEMs on this system.\n");
    return;
  }
  NFIT& nfit = *liumos->acpi.nfit;
  uint64_t available_pmem_size = 0;

  for (auto& it : nfit) {
    if (it.type != NFIT::Entry::kTypeSPARangeStructure)
      continue;
    NFIT::SPARange* spa_range = reinterpret_cast<NFIT::SPARange*>(&it);
    if (!IsEqualGUID(
            reinterpret_cast<GUID*>(&spa_range->address_range_type_guid),
            &NFIT::SPARange::kByteAdressablePersistentMemory))
      continue;
    PutStringAndHex("SPARange #", spa_range->spa_range_structure_index);
    PutStringAndHex("  Base", spa_range->system_physical_address_range_base);
    PutStringAndHex("  Length",
                    spa_range->system_physical_address_range_length);
    available_pmem_size += spa_range->system_physical_address_range_length;
    FreePages(
        pmem_allocator,
        reinterpret_cast<void*>(spa_range->system_physical_address_range_base),
        spa_range->system_physical_address_range_length >> kPageSizeExponent);
  }
  PutStringAndHex("Available PMEM (KiB)", available_pmem_size >> 10);
}

void InitMemoryManagement(EFI::MemoryMap& map) {
  InitDRAMManagement(map);
  InitPMEMManagement();
  liumos->dram_allocator = &dram_allocator_;
  liumos->pmem_allocator = &pmem_allocator_;
}

void PrintLogoFile() {
  const uint8_t* buf = logo_file.GetBuf();
  uint64_t buf_size = logo_file.GetFileSize();
  if (buf[0] != 'P' || buf[1] != '3') {
    PutString("Not supported logo type (PPM 'P3' is supported)\n");
    return;
  }
  bool in_line_comment = false;
  bool is_num_read = false;
  int width = 0;
  int height = 0;
  int max_pixel_value = 0;
  uint32_t rgb = 0;
  uint32_t tmp = 0;
  int channel_count = 0;
  int x = 0, y = 0;
  for (int i = 3; i < (int)buf_size; i++) {
    uint8_t c = buf[i];
    if (in_line_comment) {
      if (c != '\n')
        continue;
      in_line_comment = false;
      continue;
    }
    if (c == '#') {
      in_line_comment = true;
      continue;
    }
    if ('0' <= c && c <= '9') {
      is_num_read = true;
      tmp *= 10;
      tmp += (uint32_t)c - '0';
      continue;
    }
    if (!is_num_read)
      continue;
    is_num_read = false;
    if (!width) {
      width = tmp;
    } else if (!height) {
      height = tmp;
    } else if (!max_pixel_value) {
      max_pixel_value = tmp;
      assert(max_pixel_value == 255);
    } else {
      rgb <<= 8;
      rgb |= tmp;
      channel_count++;
      if (channel_count == 3) {
        channel_count = 0;
        liumos->screen_sheet->DrawRect(
            liumos->screen_sheet->GetXSize() - width + x++, y + 128, 1, 1, rgb);
        if (x >= width) {
          x = 0;
          y++;
        }
      }
    }
    tmp = 0;
  }
}

void EnableSyscall() {
  uint64_t star = GDT::kKernelCSSelector << 32;
  star |= GDT::kUserCSSelector << 48;
  WriteMSR(MSRIndex::kSTAR, star);

  uint64_t lstar = reinterpret_cast<uint64_t>(AsmSyscallHandler);
  WriteMSR(MSRIndex::kLSTAR, lstar);

  uint64_t efer = ReadMSR(MSRIndex::kEFER);
  efer |= 1;  // SCE
  WriteMSR(MSRIndex::kEFER, efer);
}

void IdentifyCPU() {
  CPUID cpuid;

  ReadCPUID(&cpuid, 0, 0);
  const uint32_t kMaxCPUID = cpuid.eax;
  PutStringAndHex("Max CPUID", kMaxCPUID);
  cpu_features.max_cpuid = kMaxCPUID;

  ReadCPUID(&cpuid, 0x8000'0000, 0);
  const uint32_t kMaxExtendedCPUID = cpuid.eax;
  PutStringAndHex("Max Extended CPUID", kMaxExtendedCPUID);
  cpu_features.max_extended_cpuid = kMaxExtendedCPUID;

  ReadCPUID(&cpuid, 1, 0);
  uint8_t cpu_family = ((cpuid.eax >> 16) & 0xff0) | ((cpuid.eax >> 8) & 0xf);
  uint8_t cpu_model = ((cpuid.eax >> 12) & 0xf0) | ((cpuid.eax >> 4) & 0xf);
  uint8_t cpu_stepping = cpuid.eax & 0xf;
  PutStringAndHex("CPU family  ", cpu_family);
  PutStringAndHex("CPU model   ", cpu_model);
  PutStringAndHex("CPU stepping", cpu_stepping);
  if (!(cpuid.edx & kCPUID01H_EDXBitAPIC))
    Panic("APIC not supported");
  if (!(cpuid.edx & kCPUID01H_EDXBitMSR))
    Panic("MSR not supported");
  cpu_features.x2apic = cpuid.ecx & kCPUID01H_ECXBitx2APIC;

  if (0x8000'0004 <= kMaxExtendedCPUID) {
    for (int i = 0; i < 3; i++) {
      ReadCPUID(&cpuid, 0x8000'0002 + i, 0);
      *reinterpret_cast<uint32_t*>(&cpu_features.brand_string[i * 16 + 0]) =
          cpuid.eax;
      *reinterpret_cast<uint32_t*>(&cpu_features.brand_string[i * 16 + 4]) =
          cpuid.ebx;
      *reinterpret_cast<uint32_t*>(&cpu_features.brand_string[i * 16 + 8]) =
          cpuid.ecx;
      *reinterpret_cast<uint32_t*>(&cpu_features.brand_string[i * 16 + 12]) =
          cpuid.edx;
    }
    PutString(cpu_features.brand_string);
    PutChar('\n');
  }

  if (CPUIDIndex::kMaxAddr <= kMaxExtendedCPUID) {
    ReadCPUID(&cpuid, CPUIDIndex::kMaxAddr, 0);
    IA32_MaxPhyAddr maxaddr;
    maxaddr.data = cpuid.eax;
    cpu_features.max_phy_addr = maxaddr.bits.physical_address_bits;
  } else {
    PutString("CPUID function 80000008H not supported.\n");
    PutString("Assuming Physical address bits = 36\n");
    cpu_features.max_phy_addr = 36;
  }
  PutStringAndHex("MAX_PHY_ADDR", cpu_features.max_phy_addr);
  cpu_features.phy_addr_mask = (1ULL << cpu_features.max_phy_addr) - 1;
  PutStringAndHex("phy_addr_mask", cpu_features.phy_addr_mask);
  liumos->cpu_features = &cpu_features;
}

void MainForBootProcessor(void* image_handle, EFI::SystemTable* system_table) {
  liumos = &liumos_;
  EFI::Init(system_table);
  EFI::ConOut::ClearScreen();
  logo_file.LoadFromEFISimpleFS(L"logo.ppm");
  hello_bin_file.LoadFromEFISimpleFS(L"hello.bin");
  liumos->hello_bin_file = &hello_bin_file;
  liumos_elf_file.LoadFromEFISimpleFS(L"LIUMOS.ELF");
  InitGraphics();
  main_console_.SetSheet(liumos->screen_sheet);
  liumos->main_console = &main_console_;
  EFI::GetMemoryMapAndExitBootServices(image_handle, efi_memory_map);
  liumos->efi_memory_map = &efi_memory_map;

  com1.Init(kPortCOM1);
  liumos->com1 = &com1;
  liumos->main_console->SetSerial(&com1);

  PrintLogoFile();
  PutString("\nliumOS version: " GIT_HASH "\n\n");
  IdentifyCPU();
  ClearIntFlag();

  ACPI::DetectTables();

  InitMemoryManagement(efi_memory_map);

  InitDoubleBuffer();
  main_console_.SetSheet(liumos->screen_sheet);

  liumos->dram_allocator->Print();
  constexpr uint64_t kNumOfKernelStackPages = 2;
  uint64_t kernel_stack_base =
      liumos->dram_allocator->AllocPages<uint64_t>(kNumOfKernelStackPages);
  liumos->dram_allocator->Print();
  PutStringAndHex("\nstack base", kernel_stack_base);
  uint64_t kernel_stack_pointer =
      kernel_stack_base + (kNumOfKernelStackPages << kPageSizeExponent);

  GDT gdt;
  gdt.Init(kernel_stack_pointer);
  IDT idt;
  idt.Init();
  InitPaging();
  keyboard_ctrl_.Init();

  ExecutionContextController exec_ctx_ctrl_;
  liumos->exec_ctx_ctrl = &exec_ctx_ctrl_;

  ExecutionContext* root_context =
      liumos->exec_ctx_ctrl->Create(nullptr, 0, nullptr, 0, ReadCR3());

  Scheduler scheduler_(root_context);
  liumos->scheduler = &scheduler_;

  bsp_local_apic.Init();
  liumos->bsp_local_apic = &bsp_local_apic;
  Disable8259PIC();

  InitIOAPIC(bsp_local_apic.GetID());

  hpet.Init(static_cast<HPET::RegisterSpace*>(
      liumos->acpi.hpet->base_address.address));
  liumos->hpet = &hpet;
  hpet.SetTimerMs(
      0, 10, HPET::TimerConfig::kUsePeriodicMode | HPET::TimerConfig::kEnable);

  GetKernelPML4().Print();

  LoadKernelELF(liumos_elf_file);
}
