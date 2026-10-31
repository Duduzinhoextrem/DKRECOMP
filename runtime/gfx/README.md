# runtime/gfx

Camada gráfica: NVN do SDK alvo, com tradução para Vulkan.

**Estado: back-end próprio inicial verificado; nenhum frame do jogo.** `vk.c` cria janela/device/swapchain,
apresenta clears, redimensiona e captura a imagem produzida pela GPU. `dkr_gfx_smoke` testa os pixels e as chamadas
com validação Khronos de sincronização. Ativar com `DKR_BUILD_GFX=ON`; dependências em `toolchain/` via
`tools/fetch_gfx_deps.py --validation`. O boot continua chegando a `nvdrv:Initialize`; ainda faltam NVN, geometria,
texturas e shaders. Nenhum código gráfico da referência foi incorporado.
Ver [avaliação e roteiro de integração](../../docs/GRAPHICS_PORT.md), com a revisão examinada, diferenças de ABI,
dependências e validações necessárias.
