#include <Uefi.h>
#include <Protocol/LoadedImage.h>

typedef struct {
    UINTN Version;
    CHAR16* ID;
    UINTN Memory_Size;

    void* Framebuffer_Base;
    UINTN Framebuffer_Size;
    UINT32 Width;
    UINT32 Height;
    UINT32 PixelsPerScanLine;
} NTBLI;

// ===== 점 찍기 =====
static inline void draw_pixel(NTBLI* info, int x, int y, UINT32 color) {
    UINT32* fb = (UINT32*)info->Framebuffer_Base;
    fb[y * info->PixelsPerScanLine + x] = color;
}

// ===== 엔트리 =====
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {

    // LoadOptions에서 NTBLI 가져오기
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    EFI_GUID lipGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

    SystemTable->BootServices->HandleProtocol(
        ImageHandle,
        &lipGuid,
        (void**)&LoadedImage
    );

    NTBLI* info = (NTBLI*)LoadedImage->LoadOptions;

    // ===== 중앙에 점 하나 =====
    int x = info->Width  / 2;
    int y = info->Height / 2;

    draw_pixel(info, x, y, 0x00FFFFFF);

    // 멈춤
    while (1);

    return EFI_SUCCESS;
}