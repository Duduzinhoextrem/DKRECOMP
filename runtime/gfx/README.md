# runtime/gfx

Camada gráfica: NVN do SDK alvo, com tradução para Vulkan.

**Estado: back-end próprio inicial verificado; nenhum frame do jogo.** `vk.c` cria janela/device/swapchain,
apresenta clears, redimensiona e captura a imagem produzida pela GPU. `dkr_gfx_smoke` testa os pixels e as chamadas
com validação Khronos de sincronização. Ativar com `DKR_BUILD_GFX=ON`; dependências em `toolchain/` via
`tools/fetch_gfx_deps.py --validation`. `nvn.c` liga o SDK ao host por overrides (`dkrecomp.gen --gfx`), cria
o contexto Vulkan e registra os nomes públicos NVN da referência MIT. Procs ausentes param por nome. O boot
agora chega a `hid:CreateAppletResource`; ainda faltam objetos/comandos NVN, geometria, texturas e shaders.
Ver [avaliação e roteiro de integração](../../docs/GRAPHICS_PORT.md), com a revisão examinada, diferenças de ABI,
dependências e validações necessárias.
