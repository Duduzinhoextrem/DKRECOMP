# runtime/gfx

Camada gráfica: NVN do SDK alvo, com tradução para Vulkan.

**Estado: avaliação iniciada; nenhum frame renderizado.** O boot chega a `nv::InitializeGraphics` e pede
`nvdrv:Initialize`. O front-end NVN, o back-end Vulkan e o tradutor de shaders ainda não foram incorporados.
Ver [avaliação e roteiro de integração](../../docs/GRAPHICS_PORT.md), com a revisão examinada, diferenças de ABI,
dependências e validações necessárias.
