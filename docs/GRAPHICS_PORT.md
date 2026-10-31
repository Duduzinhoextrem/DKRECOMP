# Integração gráfica: avaliação inicial

Estado em 2026-10-31: **nenhum front-end NVN ou back-end Vulkan foi incorporado**. O boot chega a
`nv::InitializeGraphics` (`sdk+0x264dac`) e para em `nvdrv:Initialize` (comando 3), depois de criar transfer memory
de 8 MiB. A análise abaixo é um roteiro de integração, não evidência de renderização.

## Referência examinada

Checkout local, somente leitura: `refs/totk-pc-recomp`, revisão
`c3b9a4e7f6ec3d1f0a8c84b66b1703b15639a784`. A licença principal é MIT, mas os componentes transitivos têm suas
próprias origens e avisos no `THIRD_PARTY_NOTICES.md` da referência. Não basta copiar a licença principal.

| Peça da referência | O que oferece | Adaptação necessária |
|---|---|---|
| `runtime/nvn_procs.c`, `nvn_procs.inc` | nomes de funções, endereços guest sintéticos e parada por função ainda ausente | `rt_hostfn_register`/`Cpu` do DKRecomp, registro antes de iniciar as threads |
| `runtime/nvn_api_core.c`, `nvn_api_obj.c`, `nvn_api_cmd.c`, `nvn_ext.c` | objetos NVN e gravação de comandos para replay | ABI do SDK 4.4.0, acesso a argumentos/retornos, memória e callbacks |
| `runtime/nvn_api.h`, `nvn_obj.h` | contrato de objetos e comandos do front-end | remover dependências do runtime TotK; não presumir offsets iguais |
| `runtime/vk_backend.c` e demais `vk_*.c` | execução, memória, texturas, pipelines, sincronização e apresentação Vulkan | janela, dependências, relógio, memória guest, configuração e diagnóstico do DKRecomp |
| `tools/build_xlate.py`, `tools/xlate/`, `tools/vendor/ryujinx/` | tradutor Maxwell → SPIR-V baseado no Ryujinx, com patches | compilação e licenças transitivas; cache separado dos dados do outro jogo |

O header `nvn.h` preserva aviso MIT de h1k421 e descreve tipos reconstruídos para NVN 53.105, com elementos de
uma versão posterior. Isso **não comprova compatibilidade** com os objetos usados por Tropical Freeze. O front-end
também atribui código a jhaizhou/d2r-mobile, via MacNativeGames, e a outros projetos. Preservar esses avisos se houver port.

## Fronteiras que precisam ser resolvidas

1. **CPU e chamadas.** A referência guarda vetores em `__uint128_t` no offset 512, além de outros campos que não
   estão no `Cpu` do DKRecomp. O DKRecomp usa `V128`. Não converter um ponteiro de Cpu entre os dois runtimes:
   adaptar os auxiliares de argumentos e retornos. As chamadas diretas do C gerado também precisam alcançar
   os overrides; alterar apenas `rt_lookup` não intercepta essas chamadas.
2. **Objetos NVN.** Conferir tamanhos e offsets com o SDK alvo, incluindo builders, memory pools, filas e texturas.
   Testes sintéticos de builders/getters devem preceder o uso de objetos do jogo. Não escrever o tamanho da
   struct da referência sobre uma alocação guest possivelmente menor.
3. **Escalonamento.** `NVN_BLOCKING` da referência usa `kern_core_away/back`, ausentes no DKRecomp. Hoje as threads
   usam o escalonador do Windows. Separar as esperas de GPU/apresentação da trava de SVC e avaliar a necessidade
   de núcleos virtuais se houver dependência de prioridade ou esperas circulares.
4. **Dependências.** O caminho MSYS2 de `tools/build_game.py` usa SDL3 com `libiconv` LGPL e uma variante com
   `libstdc++` GPL com exceção. Esses pacotes não devem ser adotados automaticamente sob a lista de licenças do
   DKRecomp. A própria referência oferece uma alternativa em `docs/installer.md`: SDL3 upstream e llvm-mingw/libc++.
   Verificar cada dependência efetivamente distribuída e registrá-la nos avisos locais.
5. **Tradutor.** O `build_xlate.py` examinado exige .NET 10 e ferramentas MSVC para NativeAOT. Falta Windows SDK
   nesta máquina. Avaliar primeiro uma ferramenta managed/self-contained sem NativeAOT, ou uma distribuição
   permissiva verificável, mantendo instalações dentro do workspace. Ainda não foi compilado nem testado aqui.

Há `C:\Windows\System32\vulkan-1.dll`, versão de arquivo **1.4.357.0**. Isso só confirma a presença do loader;
não substitui a criação de instância/device nem um teste de apresentação. Os headers e bibliotecas de SDL3,
Vulkan e VMA não foram encontrados dentro do checkout da referência examinado.

## Rota de implementação

Priorizar a fronteira **NVN → front-end do host → Vulkan** em vez de assumir que atender os
ioctls de `nvdrv` renderizaria por si só: a execução do NVN original também produziria comandos Maxwell que
exigiriam um interpretador/driver próprio. Esse desvio não é necessário para a rota com front-end NVN.

1. Criar uma janela e um teste Vulkan independente do jogo, com dependências locais e atribuição completa.
2. Adaptar o registro de procs e um conjunto inicial de builders/objetos, com validação da ABI alvo.
3. Criar overrides explícitos por símbolo no gerador para os pontos de entrada NVN necessários. Manter uma
   parada identificável para procs ainda ausentes; não anunciar inicialização de GPU antes de existir um back-end.
4. Integrar o front-end com memória, texturas, sincronização e comandos do back-end. Adaptar o tradutor de
   shaders e validar casos próprios antes de carregar os shaders do usuário.
5. Voltar ao boot real, registrar os nomes pedidos e seguir até a primeira apresentação. Depois vêm imagem
   correta, áudio/entrada, gameplay e só então medições de desempenho.

`refs/` permanece somente leitura. Nenhum código ou binário da referência foi copiado nesta avaliação; não há
dependência nova instalada nem marco gráfico concluído.
