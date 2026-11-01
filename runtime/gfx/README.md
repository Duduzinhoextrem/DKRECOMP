# runtime/gfx

Camada gráfica: NVN do SDK alvo, com tradução para Vulkan.

**Estado: inicialização gráfica NVN do jogo completa; comandos gravados, nenhum frame desenhado.** `vk.c` cria janela/device/swapchain,
apresenta clears, redimensiona e captura a imagem produzida pela GPU. `dkr_gfx_smoke` testa os pixels e as chamadas
com validação Khronos de sincronização. Ativar com `DKR_BUILD_GFX=ON`; dependências em `toolchain/` via
`tools/fetch_gfx_deps.py --validation`. `nvn.c` liga o SDK ao host por overrides (`dkrecomp.gen --gfx`), cria
o contexto Vulkan e registra os nomes públicos NVN da referência MIT. Procs ausentes param por nome. O boot
atinge 1.200 presents em 30 s, ainda sem imagem. `nvn_commands.c` preserva os dados transitórios;
`nvn_shaders.c` preserva código/controle e captura programas por opção. Os 100 shaders únicos de 50 programas
observados no boot traduziram e passaram no validador Vulkan 1.3; ainda falta ligar shaders/recursos/pipelines
ao replay. Os primeiros quadros já usam DrawArrays para fundos/logotipos. Não há entrada do host.
Ver [avaliação e roteiro de integração](../../docs/GRAPHICS_PORT.md), com a revisão examinada, diferenças de ABI,
dependências e validações necessárias.
