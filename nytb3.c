#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>

typedef struct {
    UINTN       Version;
    CHAR16*     ID;
    UINTN       Memory_Size;
    void*       Framebuffer_Base;
    UINTN       Framebuffer_Size;
    UINT32      Width;
    UINT32      Height;
    UINT32      PixelsPerScanLine;
} NTBLI;

EFI_STATUS HandleError(EFI_STATUS Status, CHAR16* Message)
{
    if (EFI_ERROR(Status)) {
        Print(L"Error: %s (%r)\n", Message, Status);
        gBS->Stall(3000000);
    }
    return Status;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS Status;

    // 1. Graphics Output Protocol (GOP) 얻기
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
    Status = gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID**)&Gop);
    if (EFI_ERROR(Status)) {
        return HandleError(Status, L"GOP Locate Failed");
    }

    // 2. NTBLI 구조체 할당
    NTBLI *Info;
    Status = gBS->AllocatePool(EfiLoaderData, sizeof(NTBLI), (void**)&Info);
    if (EFI_ERROR(Status)) {
        return HandleError(Status, L"NTBLI Allocation Failed");
    }

    // 3. 부트로더 정보 채우기
    Info->Version             = 3;
    Info->ID                  = L"Nyxis Bootloader Tamyo";
    Info->Framebuffer_Base    = (void*)Gop->Mode->FrameBufferBase;
    Info->Framebuffer_Size    = Gop->Mode->FrameBufferSize;
    Info->Width               = Gop->Mode->Info->HorizontalResolution;
    Info->Height              = Gop->Mode->Info->VerticalResolution;
    Info->PixelsPerScanLine   = Gop->Mode->Info->PixelsPerScanLine;

    // 4. 메모리 맵 확보 및 총 메모리 계산
    EFI_MEMORY_DESCRIPTOR *MemMap = NULL;
    UINTN MemMapSize = 0, MapKey, DescriptorSize;
    UINT32 DescriptorVersion;

    gBS->GetMemoryMap(&MemMapSize, NULL, &MapKey, &DescriptorSize, &DescriptorVersion);
    MemMapSize += (DescriptorSize * 2);

    Status = gBS->AllocatePool(EfiLoaderData, MemMapSize, (void**)&MemMap);
    if (EFI_ERROR(Status)) {
        return HandleError(Status, L"Memory Map Allocation Failed");
    }

    Status = gBS->GetMemoryMap(&MemMapSize, MemMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    if (EFI_ERROR(Status)) {
        return HandleError(Status, L"GetMemoryMap Failed");
    }

    UINTN TotalMem = 0;
    for (UINTN i = 0; i < (MemMapSize / DescriptorSize); i++) {
        EFI_MEMORY_DESCRIPTOR *Desc = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)MemMap + (i * DescriptorSize));
        TotalMem += Desc->NumberOfPages * EFI_PAGE_SIZE;
    }
    Info->Memory_Size = TotalMem;

    // 5. 현재 부트로더의 Loaded Image Protocol 얻기
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (void**)&LoadedImage);
    if (EFI_ERROR(Status)) {
        return HandleError(Status, L"LIP Handle Failed");
    }

    // 6. 커널 파일의 Device Path 생성
    EFI_DEVICE_PATH_PROTOCOL *KernelPath;
    KernelPath = FileDevicePath(LoadedImage->DeviceHandle, L"\\nyxskrnl.efi");
    if (KernelPath == NULL) {
        return HandleError(EFI_OUT_OF_RESOURCES, L"Device Path Creation Failed");
    }

    // 7. 커널 이미지 로드
    EFI_HANDLE KernelImage;
    Status = gBS->LoadImage(
        FALSE,                  // BootPolicy
        ImageHandle,            // ParentImageHandle
        KernelPath,             // DevicePath
        NULL,                   // SourceBuffer
        0,                      // SourceSize
        &KernelImage            // ImageHandle
    );
    if (EFI_ERROR(Status)) {
        return HandleError(Status, L"Kernel LoadImage Failed (nyxskrnl.efi not found?)");
    }

    // 8. 커널에 NTBLI 정보 전달 (LoadOptions 사용)
    EFI_LOADED_IMAGE_PROTOCOL *KernelLoadedImage;
    Status = gBS->HandleProtocol(KernelImage, &gEfiLoadedImageProtocolGuid, (void**)&KernelLoadedImage);
    if (EFI_ERROR(Status)) {
        return HandleError(Status, L"Kernel LIP Handle Failed");
    }

    KernelLoadedImage->LoadOptions     = Info;
    KernelLoadedImage->LoadOptionsSize = sizeof(NTBLI);

    // 9. 커널 실행
    Status = gBS->StartImage(KernelImage, NULL, NULL);
    if (EFI_ERROR(Status)) {
        return HandleError(Status, L"Kernel StartImage Failed");
    }

    return EFI_SUCCESS;
}