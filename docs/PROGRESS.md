# Progresso

Somente o que foi executado e verificado. Cada linha aponta para o comando que a reproduz (`docs/TESTING.md`).

## 2026-10-31 — Tradutor Maxwell → SPIR-V compilado e validado com shaders sintéticos

`tools/build_shader_translator.py` compila um executável managed .NET 10 usando o código MIT do TotK/Ryujinx,
com revisão fixa e nove patches aplicados apenas à cópia em `toolchain/`. O csproj inclui cinco fontes de Common,
sem seus quatro pacotes NuGet: `project.assets.json` confirma **zero bibliotecas de pacote**. Build terminou com
**zero avisos e zero erros**, sem Windows SDK/NativeAOT ou instalação global.

`shader_probe` enviou JSON inválido, chave com traversal e chave com quebra de linha, depois três jobs válidos
no mesmo processo. Os jobs geraram SPIR-V VS (**1.388 bytes**), PS (**1.028**) e CS (**700**), com estágios/reflexão
conferidos; PS tem saída RGBA e literais próprios. Nenhum código do jogo foi usado.

O validador Khronos `spirv-val` **v2026.4** foi compilado do fonte Apache-2.0, com headers fixos, usando Zig C++
local. **Os três shaders passaram em `--target-env vulkan1.3`**, além da verificação estrutural. Suítes:
**54 pytest passam, 1 pulado**; **11/11 CTest passam**. O teste de tradução é opcional quando a ferramenta não foi construída.

Isso ainda não traduz shaders reais do jogo nem desenha geometria: faltam capturar/normalizar os programas NVN,
fornecer metadados dos recursos e consumir SPIR-V/buffers/texturas no replay Vulkan. `.id` e `source.json` locais
registram os fontes usados, sem assets proprietários no Git.

## 2026-10-31 — Dados transitórios dos comandos preservados para replay

`nvn_commands.c` captura em tempo de gravação os estados, arrays de bindings, viewports/scissors/depth ranges,
cores, dados de UpdateUniformBuffer, regiões de cópia e views. SetRenderTargets copia também as views apontadas
pelo array, preservando entradas NULL. EndRecording transfere a propriedade dos payloads para o handle;
CallCommands/CopyCommands expandem os registros e retêm os dados imutáveis. Strides de cópia têm 64 bits e
acompanham o comando. Endereços GPU e objetos de recurso continuam com o tempo de vida da API.

O consumidor futuro usa `dkr_nvn_visit_commands`, em ordem, sem executar o guest novamente. O teste nativo grava
sete comandos, altera todos os argumentos originais, inclui duas vezes o handle em outro buffer, invalida o
handle original/finaliza seu buffer e confere os **14 registros**, incluindo views, valores SIMD e strides acima
de 32 bits. Passou; o teste de entrada NVN continua verde.

Boot recompilado: **1.200 presents em 30 s**, sem STOP/exceção, vigia após **2.215.570 SVCs**. Nenhuma imagem.
A captura cobre os argumentos transitórios do caminho observado; DrawTexture e CoverageModulationTable param
se chamados até medir seus layouts. Integração com lifetime dos recursos e replay Vulkan ainda pendentes.

## 2026-10-31 — Loop alcança 1.200 presents em 30 segundos, ainda sem imagem

Depois do save, faltava `nvnTextureGetZCullStorageSize`. O host informa zero armazenamento externo de ZCull:
esse estado auxiliar pertence à imagem Vulkan. Tropical Freeze ainda cria um memory pool vazio para ele;
o front-end agora preserva esse objeto lógico com bounds de tamanho zero, sem alocação GPU.

O carregamento seguinte abriu a biblioteca de vídeo (`subsdk0`, NVOMX/NvMMLite). O caminho nativo do SDK abortava
ao tentar usar nvdrv sem inicialização do driver do Switch. Devolver fd=-1, conforme nxnative, revelou um erro
no cleanup do SDK base: leitura absoluta de **0x1c48**. A abertura de NvMMLiteOpenBlock agora devolve falha antes
da alocação, e o jogo trata a indisponibilidade. **Vídeos não são decodificados nem apresentados.**

Boot de 30 s: **`nvn: present #600` e `#1200`**, sem STOP ou exceção. O vigia terminou o diagnóstico como previsto
(exit 4), após **2.259.928 SVCs**, com threads ativas/aguardando. Isso satisfaz o critério de loop da Etapa B;
não comprova título visível nem jogabilidade. A janela continua vazia, os comandos não têm replay e o áudio
capturado ainda era silencioso. Próximo passo: capturar dados apontados pelos comandos, depois replay/shaders.

## 2026-10-31 — Save cria, grava, confirma e lê dados no boot

`save.c` atende GetEntryType, CreateFile, CreateDirectory, DeleteFile/DeleteDirectory não recursivo, OpenFile,
Commit e consultas de espaço. IFile atende Read, Write, Flush, SetSize e GetSize. Read retorna quantidade efetiva
no fim do arquivo; escrever além do tamanho exige AllowAppend. Objetos IFile retêm o filesystem após fechar a sessão.

O boot criou **`save.bin`, 262.144 bytes**, gravou, fez Flush e Commit. A execução seguinte abriu e leu esse arquivo
e avançou para a inicialização de mais recursos gráficos. Nenhum save foi incluído no Git.

`romfs_storage` passou com a nova cobertura via IPC real: criação, tipos de entrada, persistência após reabrir,
leitura curta, extensão, permissões, descriptor curto, offsets inválidos, filesystem fechado antes do arquivo,
rejeição de traversal, dispositivos Win32, streams alternativos, nomes ambíguos, UTF-8 inválido e path sem terminador.
Os arquivos sintéticos ficam em uma raiz temporária e são removidos pelo próprio teste.

## 2026-10-31 — Áudio CPU do SDK abre; primeira apresentação NVN e próxima parada no save

`--gfx` agora intercepta GetWorkBufferSize/Open/Close do AudioRenderer e utiliza o servidor CPU/manual já
presente no SDK 4.4. O gerador preserva um trampoline para a implementação original de cada override, acessível
por `rt_call_original`; chamadas normais continuam chegando ao host. Teste sintético confere delegação direta e indireta.

| Evidência | Resultado |
|---|---|
| Boot incremental de 30 s | Para em **IFileSystem(save):GetEntryType (7)**, após **1.251.486 SVCs**, em aproximadamente 6 s |
| Armazenamento do renderer | Jogo recebe **0x9e000**; servidor CPU precisa de **0xa0000**, alocado pelo host em região guest rastreada |
| Thread/evento | Thread criada pelo próprio SDK, com TLR/ThreadType válidos; evento local substituído por handles do mesmo evento de kernel |
| Saída Windows | WinMM abriu **48 kHz estéreo, 4 × 240 amostras**; captura registrou **200 quadros do SDK, todos silenciosos** |
| Gráficos | Primeira chamada **`nvn: present #1`**; **nenhuma imagem**, pois replay/shaders ainda não existem |
| Recursos posteriores | Log chegou a **Texture #1431**, depois do antigo bloqueio de áudio |

`audio_renderer` também confere captura DeviceSink SDK 4: estéreo, clipping, downmix de seis canais, índice inválido,
buffer curto, comando desativado e lista truncada. O teste não comprova som do jogo. Close/reabertura e estabilidade
prolongada ainda precisam ser exercitados. Pesquisa e fontes: `PLAYABLE_RESEARCH.md`.

## 2026-10-31 — SIMD do renderizador CPU de áudio

O boot com o servidor CPU do SDK executou `SQRSHRN` e `SQADD`, antes ausentes. Foram implementadas as formas
vetoriais SQSHRN/SQRSHRN/UQSHRN/UQRSHRN, incluindo a variante que escreve a metade alta, e SQADD/UQADD.
A flag FPSR.QC é acumulada. Arredondamento evita overflow dos operandos de 64 bits.

`tests/diff/saturating_diff.py`: **924 codificações, 14.784 casos, zero diferenças** contra Unicorn, com limites,
aliases e QC inicialmente limpo/ativo. `insn_diff.py --seed 617 --count 2000 --states 3 --only vec_elem`:
**6.000 casos, zero diferenças**, incluindo **2.376** casos das novas multiplicações longas por elemento.
O tratamento de MUL inteiro por elemento de 32 bits também passou a ser alcançável antes da classe FP.

Suíte Python completa: **53 passam, 1 pulado** (`reports/pytest_audio.txt`, 47,21 s).

## 2026-10-31 — Work buffer do áudio REV4 aceito; próximo é OpenAudioRenderer

`runtime/src/audio.c` atende `audren:u:GetWorkBufferSize` (1), com parâmetro interno de 0x34 bytes, revisão em
+0x30 e voice-drop de um byte. Fórmula REV1–4 adaptada da referência MIT nxnative, com origem/licença preservadas;
não usa a necessidade maior do servidor CPU interno do SDK como se fosse a necessidade do serviço DSP.

| Evidência | Resultado |
|---|---|
| Boot incremental | SDK aceita **0x9e000 bytes**, cria transfer memory e pede **OpenAudioRenderer (0)**; parada mais recente em **3102 SVCs** (primeira execução, 2829) |
| Parâmetro do jogo | REV4, 48 kHz, 240 samples, 54 mix buffers, 2 submixes, 128 vozes, 2 sinks, 12 efeitos, 12 frames de performance |
| Próximo pedido | 72 bytes úteis (parâmetro, padding, work size, ARUID 0x10), PID, copy0=transfer memory, copy1=processo; buffers ausentes |
| `audio_renderer` | fixture REV4 do serviço **0x308000**, IPC e rejeição de revisão, campos negativos, limites e truncamento; padding do voice-drop tratado separadamente |
| CMake/ctest; pytest | **11/11 passam**; **52 passam, 1 pulado** |

O áudio ainda não foi aberto, processado nem reproduzido. Etapa A continua com Open/RequestUpdate, progresso
silencioso das vozes e evento periódico. O diagnóstico IPC agora inclui os handles e até 96 bytes de argumentos.

## 2026-10-31 — Estado NVN atual revalidado e builders comparados com o SDK

Após a integração dos objetos NVN, build incremental + boot confirmam a parada posterior em **`audren:u`
comando 1 (GetWorkBufferSize)**, desta vez em **2615 SVCs**. O renderer solicitado tem 48 kHz, 240 amostras e
revisão REV4. A contagem varia com o escalonamento; o ponto de parada coincide com o avanço inicial de 2876 SVCs.

`dkrecomp.testing.nvn_probe` compara o bridge nativo com o SDK original sob Unicorn: **512 casos, zero diferenças**
para DeviceBuilderSetDefaults/SetFlags, incluindo regiões aleatórias e guardas de escrita. O resolver e a guarda
de inicialização são informados pela CLI; a guarda só é marcada no emulador, sem executar a inicialização GPU.
Relatório local: `reports/nvn_builder_probe.json`; imagem SHA-256 `5ce080b39a770f512fac8b1b77a21d7eb057dbabf2e23c5d63e26482597b0766`.

Suítes atuais executadas: **10/10 CMake/ctest**, **52 testes Python passam, 1 pulado**. Nenhum frame do jogo.

## 2026-10-31 — Inicialização gráfica NVN completa (sem renderização); boot chega ao áudio

O driver NVN original está dentro do `sdk`. `dkrecomp.nvnmap` lê a tabela nome→rotina dele (477 nomes,
`sdk+0x2e01e0` busca binária, tabela estática pela GOT `sdk+0x99b808`). Com isso os layouts de builders e
objetos foram **medidos nas rotinas originais**, não copiados do NVN 55 do TotK. A análise de pureza separa as
rotinas que só usam argumentos e dados constantes: **211** procs (builders, estados, views, encoders de handle,
modos do device) passam a rodar o código original recompilado. A primeira versão da análise aceitava ponteiros
lidos da memória e deixou passar `nvnTextureBuilderGetStorageSize`, que segue `builder->device` e lê estado
interno do device: o boot falhou ali e a análise passou a rastrear a origem dos ponteiros (teste novo).

O host implementa device, sync, memory pools, buffers, eventos, filas, texturas (layout block-linear do Tegra,
adaptado do deko3d via TotK Recomp), samplers, pools de descritores com tabela de registros, programas
(guardam o shader como o jogo entrega), command buffers e janela. **Modelo de GPU atual:** os comandos são
gravados (proc + registradores de argumento) e aceitos no submit; ainda não existe replay Vulkan nem tradutor
de shaders, então **nada é desenhado**. O present só cadencia a 60 Hz/intervalo e conta quadros.

| Evidência | Resultado |
|---|---|
| Build incremental + boot de 30 s (`DKR_GFX_HIDDEN=1 tools/boot.sh 30`) | **2876 SVCs** (antes 416). Device API 53.105, fila, 9 memory pools, pools de 4096 samplers e 8448 texturas, **32 texturas** (render targets 1920×1080 RGBA8 de 0x870000 bytes), janela de 2 texturas 1920×1080 com intervalo 1. Para em **`audren:u` comando 1** (GetWorkBufferSize) |
| `nvn_texture_layout` | **40/40** casos de tamanho/altura de bloco medidos no SDK original (NVN 55), pitch e layout empacotado |
| `test_nvnmap.py` | setter puro; `builder->device->campo`, global gravável, `br` e ponteiro lido passado a call são impuros |
| CMake/ctest; pytest | **10/10 passam**; **52 passam, 1 pulado** |

Crashes do host agora são relatados com registradores e frames do guest (`crash_report` em `horizon.c`),
em vez de terminar em silêncio.

## 2026-10-31 — Binder Connect aceito; primeira chamada NVN alcançada

O parser de parcels valida limites/offsets e o token UTF-16 `android.gui.IGraphicBufferProducer`. Aceita os
buffers X/C de TransactParcelAuto e A/B do formato legado. Connect registra API/controle do produtor e devolve
o modo lógico e status Android; uma segunda conexão retorna BAD_VALUE. Disconnect confere a API antes de liberar.
SetLayerScalingMode guarda o valor solicitado para a futura adaptação de apresentação.

| Evidência | Resultado |
|---|---|
| Build incremental + boot de 30 s | **416 SVCs**; SDK aceita Connect **API=2**, app-controlled=0 e escala **2**; jogo carrega **475 nomes NVN conhecidos** e chega à primeira chamada **`nvnDeviceBuilderSetDefaults`**, ainda não implementada |
| Mesmo boot com camada Khronos + sincronização | **416 SVCs**, mesma parada e sem erros Vulkan |
| `display_service` | Connect/Disconnect por X/C e A/B, conexão duplicada/API errada, bounds/canaries de saída, offsets/tamanho inválidos, token UTF-16 inválido e argumentos truncados; entradas rejeitadas não alteram a conexão |
| CMake/ctest | **8/8 passam**; último pytest integral permanece **51 passam, 1 pulado**, sem novas mudanças Python neste passo |

O carregamento consultou também `nvnDeviceApplyDeferredFinalizes` e `nvnQueueAcquireTexture`, ausentes da lista
de nomes mais nova: retornaram NULL. Ainda não foram chamados no caminho medido. Próximo: conferir/adaptar
layouts e semântica dos builders/objetos ao NVN do alvo. Dequeue, QueueBuffer e apresentação do jogo continuam pendentes.

## 2026-10-31 — Display e NativeWindow aceitos pelo SDK; próximo é Binder Connect

`runtime/src/vi.c` atende as raízes `vi:u/s/m`, os privilégios dos serviços de display, a abertura/fechamento
do display lógico Default (1280×720), resolução e abertura/fechamento de layers gerenciados. O applet cria uma
identidade de layer rastreada, em lugar do antigo ID constante. OpenLayer escreve um parcel NativeWindow de
0x3c bytes, com Binder e tabela de objetos, respeitando o tamanho do buffer.

O **SDK original desserializou esse parcel** e chamou AdjustRefcount para weak/strong refs e GetNativeHandle
0xf. O evento retornado é de disponibilidade da fila inicialmente vazia, com referência compartilhada;
não é um evento de VSync nem de conclusão GPU. Transações da fila ainda param explicitamente.

| Evidência | Resultado |
|---|---|
| Build incremental + boot de 30 s | **415 SVCs**; display 1, layer 1/Binder 1, weak=1/strong=1; próximo **`vi:IHOSBinderDriver:TransactParcelAuto` (3), código 10 (Connect)** |
| Mesmo boot com camada Khronos + sincronização | **415 SVCs**, mesma parada e sem erros Vulkan |
| `display_service` | hierarquia/privilégios, display/resolução, limites do parcel, identidade dos layers, refcounts sem underflow, eventos copiados e fechamento |
| CMake/ctest; pytest | **8/8 passam**; **51 passam, 1 pulado** |

Connect chega com entrada X de **0x6c** bytes e saída C de **0x1000**; descritores A/B alternativos estão vazios.
Implementação própria baseada no protocolo público do switchbrew/libnx; nenhum código de fila de terceiros copiado.
Ainda não há compositor, transações de buffers nem frame do jogo.

## 2026-10-31 — Configuração HID concluída no caminho de boot; próximo serviço é VI

HID passa por ActivateNpad, orientação dos Joy-Con, modo handheld, IDs e estilos suportados e criação/ativação
da lista de dispositivos de vibração. A configuração fica armazenada; rings de Npad são válidos e vazios.
Sem back-end de entrada conectado, nenhum botão nem rumble é gerado. Getters e eventos de estilo também têm
tratadores para esse estado desconectado. IDs conferidos no SDK e libnx: Handheld=0x20, Other=0x10.

Eventos copiados agora retêm o mesmo objeto, inclusive sinalização, reset e wakeups de waiters em outro handle.
Quando dois handles do mesmo evento estão no array de espera, vence o primeiro índice, não o menor handle.

| Evidência | Resultado |
|---|---|
| Build incremental e boot de 30 s | **403 SVCs**, nove threads guest iniciadas; HID aceita slots **0x1ff**, estilos **0x1f** e ativa 18 handles na lista de vibração; parada em **`vi:m:GetDisplayService` (2)** na thread `0xc008` |
| Mesmo boot com camada Khronos + sincronização | **403 SVCs**, mesma parada, camada ativa e sem erros Vulkan |
| `shared_memory` e `kernel_sync` | configurações/getters HID, limites, IDs dos slots, eventos copiados, reset, referências e wakeup/índice de aliases conferem |
| CMake/ctest; pytest | **7/7 passam**; **51 passam, 1 pulado** |

O próximo trabalho é o serviço de display/VI e os objetos NVN, com a entrada do host ainda pendente. Nenhum frame
do jogo foi apresentado.

## 2026-10-31 — Memória compartilhada real do HID mapeada pelo SDK

`runtime/src/shared.c` cria sections do Windows com uma view de escrita do serviço e aliases guest protegidos.
Placeholders divididos permitem endereços alinhados a 4 KiB, preservando a granularidade de reserva de 64 KiB.
Handles copiados e mappings retêm o objeto; unmap devolve o intervalo e fechar um handle não invalida aliases.
SVCs 0x13/0x14 atualizam também o estado Shared e as permissões de `QueryMemory`.

HID implementa `CreateAppletResource` (0) e `IAppletResource:GetSharedMemoryHandle` (0). O bloco de 256 KiB
começa zerado, com controles desconectados. Ativação e publicação dos estados de entrada ainda estão pendentes.

| Evidência | Resultado |
|---|---|
| `tools/boot.sh 30` com `DKR_GFX_HIDDEN=1` | **329 SVCs**; SDK mapeia HID em **`0x7c70a04000`**, tamanho **0x40000**, permissão de leitura; próximo comando **`hid:ActivateNpad` (103)** |
| `shared_memory` | aliases reais no mesmo bloco de 64 KiB, propagação nos dois sentidos, read-only, limites/alinhamento, handles, remapeamento, 100 ciclos e IPC HID |
| CMake/ctest; pytest | **7/7 passam**; **51 passam, 1 pulado** |

O alocador de shared memory não substitui reservas privadas preexistentes nem atravessa duas reservas
independentes de placeholders. Uma escolha guest que exija isso falha explicitamente; não há cópia silenciosa.
As páginas de guarda restantes de uma reserva compartilhada não são commitadas pelo alocador privado atual.
Nenhum frame do jogo foi submetido.

## 2026-10-31 — Fronteira NVN ligada ao boot: próximo serviço é HID

O gerador permite overrides por símbolo, inclusive aliases: substitui o corpo da função recompilada para que
chamadas C diretas e despacho pela tabela alcancem o mesmo tratador do host. O manifesto separa os overrides
das instruções efetivamente recompiladas. `--gfx` liga essa fronteira e o back-end Vulkan ao executável.

`runtime/gfx/nvn.c` registra 537 nomes públicos de API antes das threads; a lista MIT da referência foi copiada
com atribuição/licenças. Há lookup e `nvnDeviceGetProcAddress`; operações ainda não implementadas param por
nome quando chamadas. `nv::InitializeGraphics` cria o device/swapchain Vulkan reais e registra a memória guest.
Não devolve um falso frame nem executa comandos de GPU ainda ausentes.

| Evidência | Resultado |
|---|---|
| Teste `test_host_override_executes_for_direct_and_indirect_calls` | C ARM64 sintético recompilado chama o override por BL e por tabela; argumentos/resultados e registradores preservados conferem, inclusive quando o símbolo configurado é alias |
| `tools/boot.sh 30 --gfx` | **321 SVCs**, sete threads guest iniciadas; device/swapchain Vulkan 1280×720 criados na RTX 5060 Ti; parada em **`hid:CreateAppletResource` (0)** |
| Helper padrão, com camada Khronos e sincronização ativadas no ambiente do processo | **323 SVCs**, mesma parada HID, sem mensagens de erro de validação; a contagem varia com o escalonamento |
| `tools/boot.sh 30 --no-gfx` | caminho legado recompilado e executado: **292 SVCs**, parada em `nvdrv:Initialize` |
| `pytest`; CMake/ctest gráfico | **51 passam, 1 pulado**; **6/6 passam** |

O jogo ainda não submeteu frames. O próximo trabalho do boot é HID/memória compartilhada, seguido dos objetos e
comandos NVN à medida que forem chamados. `tools/boot.sh` passa a usar gráficos por padrão; `--no-gfx` mantém o
caminho SVC/nvdrv anterior para diagnóstico. A geração direta de módulos continua exigindo `--gfx` explicitamente.

## 2026-10-31 — Janela e apresentação Vulkan verificadas na GPU

Novo back-end próprio em `runtime/gfx/vk.c`, com API `dkr/gfx.h`: loader dinâmico do Windows, instância/device,
janela Win32, swapchain, comandos de clear, apresentação, resize e readback para BMP. Independente do jogo e
do estado da CPU guest. Headers Khronos fixados por revisão/SHA-256; camada de validação local, sem instalar SDK.

| Evidência | Resultado |
|---|---|
| `dkr_gfx_smoke` | RTX 5060 Ti, API do device **1.4.357**, família de fila 0, swapchain de três imagens BGRA8 UNORM |
| `vulkan_present` | **8 apresentações**, janela 640×360 → 320×240; os **76.800 pixels** do readback conferem com a cor pedida |
| `vulkan_validation` | **32 apresentações**, resize e readback; **0 erros**, com `VK_LAYER_KHRONOS_validation` e validação de sincronização ativas, inclusive teardown |
| Execução visível `--hold-seconds 30` | janela nativa inspecionada por captura; área cliente azul preenchida; programa termina com exit 0 |
| CMake/ctest com `DKR_BUILD_GFX=ON` | **6/6 passam**, incluindo os quatro testes anteriores |

A validação de sincronização detectou uma corrida entre a aquisição da imagem e sua transição de layout.
Corrigida fazendo o escopo de origem da barreira corresponder ao estágio esperado pelo semáforo de aquisição.
Os semáforos de apresentação são individuais por imagem da swapchain. Erros gerais do loader sobre overlays
antigos do sistema são registrados separadamente; não são classificados como erros de validação da aplicação.

**O jogo ainda não usa esse back-end.** O boot continua parando em `nvdrv:Initialize` (292 SVCs). Este teste
prova apresentação/clear/readback, não desenho de geometria, shaders, NVN ou imagem do jogo.

## 2026-10-31 — Montagem do save: inicialização gráfica alcança nvdrv

`fsp-srv:OpenSaveDataFileSystem` (51) cria/abre a pasta do save de conta sob
`user_files/saves/<program-id>/<uid>/` e devolve uma IFileSystem. A criação verifica os componentes do caminho e
recusa reparse points; não há operações de arquivo implementadas ainda. `DKR_SAVE_ROOT` seleciona outra raiz.
Objetos IPC dinâmicos agora têm contagem de referências e destrutor, inclusive quando compartilhados por domínios.

`CreateTransferMemory` (SVC 0x15) registra um intervalo guest existente, tamanho e permissão num handle, que pode
ser consultado por serviços e fechado. Valida alinhamento, tamanho e permissões; não aplica proteção de páginas
ou os atributos de memória do console.

| Evidência | Resultado |
|---|---|
| Build nativo + boot de 30 s | **292 SVCs**: monta o save, inicia `0xc002` e `0xc003` além da `0xc001`, cria transfer memory de **8 MiB**; `nv::InitializeGraphics` pede **`nvdrv:Initialize` (3)**, onde para por serviço ausente |
| CMake/ctest | **4/4 passam**; teste de save cria e remove somente as próprias pastas temporárias; teste IPC verifica destruição do objeto após fechar o último clone e fechamento de transfer memory |

O novo limite do boot é a implementação do driver gráfico. A montagem do save não prova leitura/escrita de save;
nenhuma delas foi solicitada pelo caminho executado até aqui. Ainda não há apresentação de frames, áudio ou entrada.

Avaliação gráfica inicial: `docs/GRAPHICS_PORT.md` registra a revisão TotK examinada, as diferenças de Cpu/ABI,
o contrato do front-end NVN, dependências e uma rota de integração Vulkan. Só documentação/inspeção: nenhum
código de terceiros foi incorporado. O loader Vulkan do Windows existe (versão de arquivo 1.4.357.0), mas
nenhum device ou teste de apresentação foi executado.

## 2026-10-31 — Usuário pré-selecionado: boot chega ao save

`IApplicationFunctions:PopLaunchParameter(2)` entrega o parâmetro de usuário local por
`am:IStorage` → `IStorageAccessor`. O SDK 4.4.0 lê 0x88 bytes com magic, flag e UID; é o mesmo UID usado por
`acc:u0`. O parâmetro é consumido uma vez. A ausência do parâmetro de UserChannel não consome o de usuário.

| Evidência | Resultado |
|---|---|
| Build nativo + boot de 30 s | **267 SVCs**; o SDK obtém o parâmetro, o jogo abre o usuário, chama `EnsureSaveData` e pede **`fsp-srv:OpenSaveDataFileSystem` (51)**; parada explícita nesse comando |
| CMake/ctest | **4/4 passam**; `romfs_storage` também verifica o parâmetro por IPC, seu tamanho, magic/flag, UID igual ao de `GetLastOpenedUser`, limites de leitura e consumo único |

O abort anterior (`OpenPreselectedUser`, 2162-0001) deixou de ocorrer nesse caminho. A abertura real de save
ainda está pendente; `EnsureSaveData` já era aceito pelo runtime. Nenhuma tela nem gameplay foi produzido.

## 2026-10-31 — Threads do host e RomFS extraída

`runtime/src/kernel.c` executa cada thread guest em uma thread do Windows, com CPU/TLR próprios, uma trava
do kernel e esperas que liberam essa trava. Implementa transferência de mutex por prioridade/FIFO, condições,
eventos, espera pela saída de threads, sleep e relatório das threads pelo vigia. Clones de uma sessão IPC
compartilham os objetos do domínio; fechar uma sessão mantém o domínio enquanto houver outro clone.

| Evidência | Resultado |
|---|---|
| Boot antes de ligar a RomFS | **237 SVCs**; a thread `0xc001` executa código do SDK em paralelo; `fsp-srv` atende 1 e 1005; parada no comando **200**, `OpenDataStorageByCurrentProcess` |
| `ctest --test-dir build/zig --output-on-failure` | **4/4 passam**: auxiliares da CPU, IPC (inclui clone de domínio), kernel e IStorage com arquivo sintético |
| `kernel_sync` | 6 threads fazem **12.000 incrementos** protegidos por mutex; condições testam prioridade, sinal individual, broadcast, timeout e aquisição do mutex após sinal; eventos e espera pela saída de threads passam |
| Extração local com `nstool`, seção `/1/` do NCA de programa base | **83 arquivos**, **7.097.569.430 bytes**, em `user_files/romfs/` |
| `dkrecomp.romfs user_files/romfs user_files/romfs.bin --force` | **13 diretórios contando a raiz**, 83 arquivos; imagem de **7.097.575.296 bytes** |
| `nstool -t romfs --fstree user_files/romfs.bin` | parser independente aceita a imagem e lista os **83 arquivos**, com 12 diretórios além da raiz |
| Comparação por SHA-256 das regiões de dados da imagem com os arquivos extraídos (Python; `reports/romfs_verify.json`) | **83/83 arquivos conferem**, 7.097.569.430 bytes comparados |
| `pytest` | **50 passam, 1 pulado** (criação de symlink indisponível neste Windows) |

O teste de kernel reproduziu e orientou duas correções: uma condição sinalizada não pode expirar enquanto
re-adquire o mutex; `WaitSynchronization` deve rejeitar os pseudo-handles de thread/processo. A aquisição do
mutex ao sinalizar uma condição também usa CAS para não perder uma liberação feita simultaneamente pelo guest.

`runtime/src/fs.c` fornece o armazenamento somente leitura da RomFS, com offsets de 64 bits e validação dos
limites da leitura. `tools/dkrecomp/romfs.py` monta uma imagem crua a partir de arquivos já extraídos, sem
decriptação, e escreve por streaming. Os metadados ficam depois dos dados, como nas imagens do Switch. Os
arquivos extraídos, a imagem e os relatórios continuam ignorados pelo Git. O Ninja gerado agora acompanha
dependências de headers para evitar um executável com partes do runtime desatualizadas.

Primeiro boot com IStorage: o log registra **`boot: entered nnMain at main+0x254`**, abre a imagem e lê os
0x50 bytes do header; na SVC 239 aparece um salto indireto não descoberto para `main+0x3960`. O endereço está
na tabela `.eh_frame_hdr`; o gerador passou a usá-la como fonte de inícios de função, com teste sintético de
callback alcançável somente por ponteiro. Os chunks C mantêm seus limites entre gerações para evitar recompilar
todos os arquivos quando surgem novas funções. O teste também cobre a migração da saída anterior para esse layout.

**Boot após a correção de descoberta**: **250 SVCs**, `nnMain` confirmado, header e quatro tabelas da RomFS
lidos (offsets `0x1a70c52c8` a `0x1a70c55f4`, acima de 4 GiB). O próprio guest chama `Break`, após
`SetTerminateResult(0x2a2)` (`2162-0001`). Os frames guest localizam a parada em `CUser::Open` →
`nn::account::OpenPreselectedUser`: o UID pré-selecionado está vazio, pois `PopLaunchParameter(2)` ainda devolve
ausência de parâmetro. Próximo passo: fornecer esse parâmetro de lançamento pelo applet manager. A RomFS já
foi montada; não há gráfico nem gameplay. Geração atual: main 59.655, subsdk0 13.329, sdk 28.224 funções.

## 2026-10-31 — IPC e serviços: o boot passa pelos construtores e chega à primeira thread

Novo: `runtime/src/ipc.c` (mensagens HIPC/CMIF: cabeçalho, PID e handles, descritores X/A/B/W/C, `SFCI`/`SFCO`,
domínios, mensagens de controle, tabela de sessões, eventos), `runtime/src/services.c` (serviços) e
`tools/boot.sh` (regenerar, compilar e rodar um boot). Um serviço desconhecido é entregue pelo `sm:` e para a
execução no primeiro comando, com relatório; assim cada boot aponta o próximo comando a implementar.

| Evidência | Resultado |
|---|---|
| `dkr_ipc_test` (CMake): mensagem real do `sm:Initialize`, `GetService`, descritores com bits de endereço divididos, domínio, respostas | passa |
| Boot (`tools/boot.sh`) | **220 SVCs**: 88 mensagens IPC atendidas a 11 serviços; **os construtores (`DT_INIT`) de `main`, `subsdk0` e `sdk` rodam e retornam**; para em `CreateThread` (primeira thread extra, entrada em `sdk+0x95aac`) |

Serviços implementados (só os comandos pedidos no boot, mais vizinhos óbvios; comandos ids do switchbrew,
firmware 4.x): `sm:`; `lm` (logs do jogo vão para o log do boot); `appletOE` com `IApplicationProxy`,
`ICommonStateGetter` (fila de mensagens com `FocusStateChanged`, modo TV, foco), `ISelfController`,
`IWindowController`, `IAudioController`, `IApplicationFunctions`; `pctl`/`pctl:s`/`pctl:r`/`pctl:a` (sem
restrições); `set` (15 idiomas do firmware 4.x, região EUA); `apm`; `acc:u0` (um usuário local, perfil);
`time:u`/`time:s` (relógio do Windows, UTC). Também: eventos do kernel, `WaitSynchronization`, `MapMemory`/
`UnmapMemory` (cópia, equivalente ao alias porque a origem fica inacessível), `SetHeapSize` limitado à memória
total (o SDK sonda o máximo e converge para `0xC5C00000`, como num console).

Bugs encontrados no caminho: `BL` para função estática dentro do intervalo de outra função não virava início de
função (corrigido na descoberta); heap aceitava até 6 GB (corrigido com o limite de memória).

## 2026-10-31 — Primeiro boot: o SDK do jogo inicializa até a primeira mensagem IPC (MVP 5)

Novas peças: `tools/dkrecomp/link.py` (layout e ligação estática dos módulos, o trabalho do `rtld` feito na geração),
`dkrecomp.gen` para a ExeFS inteira (imagens ligadas, uma tabela de despacho por módulo, `layout.c`, um
`build.ninja`), descoberta de funções por ponto fixo (desvios diretos para fora da função, ponteiros relocados e
endereços calculados por `ADR`/`ADRP+ADD`), `runtime/src/loader.c` e `runtime/src/horizon.c` (espaço de endereços,
thread principal, callbacks do `rtld`, SVCs com log).

**Sequência de boot, lida do `rtld` do próprio jogo:** `nn::init::Start(handle_da_thread_principal,
endereço_logo_após_o_rtld, cb_flag, cb_inits)`. `cb_flag` grava 1 numa variável do `rtld`; `cb_inits` percorre a
lista de módulos chamando a inicialização de cada um. O SDK 4.4.0 passa 2 callbacks (o SDK do TotK, 3).

| Medida (ExeFS 1.0.0) | Resultado |
|---|---|
| Módulos | `main` @ `0x800000000` (100 MiB), `subsdk0` @ `0x806600000` (7 MiB), `sdk` @ `0x806e00000` (10,5 MiB) |
| Relocações | só `RELATIVE`, `ABS64`, `GLOB_DAT`, `JUMP_SLOT`; não resolvidos: só símbolos internos do `rtld` e um fraco opcional |
| Funções geradas | `main` 59.632, `subsdk0` 13.236, `sdk` 28.109 |
| Geração / compilação (12 processos) / executável | ~40 s / ~9 min / `dkctf.exe` de 106 MB |
| **Boot** | `nn::init::Start` roda; 18 SVCs atendidas (GetInfo de entropia, regiões, memória; QueryMemory; prioridade/ID da thread; SignalProcessWideKey; ConnectToNamedPort `"sm:"`); callback 1 do `rtld` chamado; **para na 19ª: primeira mensagem IPC ao `sm:`** (cabeçalho `00000004 8000000a 00000001…`, pedido com PID: `sm:Initialize`) |

Confirmado no boot: `x1` é a base do primeiro módulo (o SDK consulta exatamente `0x800000000` com QueryMemory).
Primeira tentativa: parou num salto de cauda para um stub de PLT não descoberto; levou à descoberta por ponto fixo.

## 2026-10-31 — O `main` inteiro do jogo compila e liga

Nova ferramenta `tools/dkrecomp/gen.py` (`python -m dkrecomp.gen`): gera o C de um módulo NSO inteiro, dividido em
arquivos, com a tabela de despacho e um `build.ninja`; `runtime/src/probe_main.c` liga um executável de sonda que
confere a tabela sem executar código do jogo.

| Medida (ExeFS 1.0.0, `main`, base provisória `0x800000000`) | Resultado |
|---|---|
| Funções geradas | 58.606 (57.776 com nome + 830 alvos de `BL` sem símbolo) |
| Instruções recompiladas | 2.987.362, **0 não suportadas** |
| Tempo de geração (Python, 1 processo) | 23,5 s |
| C gerado | 310 MB em 99 arquivos (maior: 7,9 MB) |
| Compilação (Clang do Zig, `-O1`, 12 processos, Ryzen 7 5700X) | 276 s, 0 erros; memória ~3 GB no total, ≤ 0,4 GB por processo |
| Objetos / executável de sonda | 439 MB / 65 MB |
| Sonda: tabela ordenada e cada entrada encontrável por `rt_lookup` | 58.606 entradas, 0 erros |
| Forma do código | 177.127 chamadas diretas, 37.249 chamadas por `rt_dispatch`, 5.725 funções com despacho local de `BR`; maior função com 29.862 linhas de C |

**O que isso prova e o que não prova:** o gerador aguenta o tamanho real e o C gerado é válido para o compilador.
Nenhuma linha do jogo foi executada: falta carregar o módulo na memória (segmentos, relocações, `.bss`), ligar com
o `sdk`, criar threads/TLS e implementar SVCs e serviços (MVP 5).

## 2026-10-31 — SIMD vetorial: `main` 100% coberto

Novo: `runtime/include/dkr/simd.h` e as classes vetoriais no decodificador/lifter:
- FP vetorial (FADD, FSUB, FMUL, FDIV, FMAX/FMIN(NM), FABD, FADDP/FMAXP/FMINP, FMULX, FMLA/FMLS, comparações
  com registrador e com zero, FABS, FNEG, FSQRT, conversões);
- estimativas FRECPE/FRSQRTE (algoritmo da Arm) e passos FRECPS/FRSQRTS;
- por elemento: FMUL/FMULX/FMLA/FMLS (vetor e escalar), MUL;
- inteiros: ADD, SUB, MUL, MLA/MLS, ADDP, comparações, CMTST, REV16/32/64, CNT, NOT, ABS, NEG, SHL/SSHR/USHR,
  SSHLL/USHLL(2), XTN(2), [SU]ADDL/SUBL/MLAL/MLSL/MULL(2), ADDV, ADDP escalar;
- BSL/BIT/BIF; ZIP/UZP/TRN; EXT; TBL/TBX; DUP e INS de elemento;
- MOVI/MVNI/ORR/BIC/FMOV vetoriais (todos os modos de imediato);
- LD1/ST1 de uma lane e LD1R, com pós-incremento;
- a palavra de armadilha `0xE7FFDEFE` do compilador da Nintendo, tratada como trap.

| Evidência | Resultado |
|---|---|
| `insn_diff --only vec_`, sementes 61–64 e 72, 6–8 mil instruções | 146.000 casos, 0 falhas |
| Teste de mutação SIMD: arredondamento do RecipEstimate, ZIP, NaN em comparação, SSHR sem sinal | as 4 detectadas (25, 438, 32, 32 casos) |
| `func_diff --vectorize` (programas vetorizados pelo Clang), O0/O1/O2/Os | 416 execuções, 0 falhas (Unicorn = recompilado = nativo) |
| `func_diff` sem vetorização, regressão inteira (sementes 11–12) e FP (semente 33) | 0 falhas |
| `pytest` | 39 testes passam |

Bug encontrado: `FRSQRTS` estourava para infinito perto do limite do float, porque 3 − a·b era arredondado antes
da divisão por 2. Corrigido dividindo um operando normal por 2 antes (exato), mantendo um só arredondamento.

**Cobertura no jogo (1.0.0)**:

| Módulo | Funções 100% suportadas | Instruções suportadas |
|---|---|---|
| `main` | 99,5% → **100%** (57.776 / 57.776) | 99,89% → **100%** |
| `subsdk0` | 99,9% → **100%** | **100%** |
| `sdk` | 99,3% → 99,6% (67 funções faltando) | 99,79% |

O que resta é só no `sdk`: AES, SHA-1, SHA-256, PMULL, CRC32, `CNTPCT_EL0`, `CTR_EL0` e algumas formas
saturadas (`sqadd`, `sqrshrn`).

## 2026-10-30 (noite) — Ponto flutuante escalar

Novo: `runtime/include/dkr/fp.h` e regras no decodificador/lifter para FP escalar (single e double): aritmética
(FADD, FSUB, FMUL, FDIV, FNMUL, FMAX/FMIN/FMAXNM/FMINNM, FMADD/FMSUB/FNMADD/FNMSUB fundidos), FABS, FNEG, FSQRT,
FMOV (registrador e imediato), FCVT entre single e double, FRINT N/P/M/Z/A/X/I, FCMP/FCMPE, FCCMP/FCCMPE, FCSEL,
conversões FP↔inteiro em todos os modos de arredondamento (com saturação) e em ponto fixo, SCVTF/UCVTF escalares
SIMD.

| Evidência | Resultado |
|---|---|
| `insn_diff --only fp_`, sementes 31–33, 8.000 instr. × 4 estados | 96.000 casos, 0 falhas |
| `insn_diff --only fp_`, semente 51 (com ponto fixo), 6.000 × 4 | 24.000 casos, 0 falhas |
| Teste de mutação FP: NaN padrão, prioridade de SNaN, saturação, FMA não fundido | as 4 detectadas (108, 13, 37 e 19 casos) |
| `func_diff` O0/O1/O2/Os com `fp_physics` (float/double, sqrt, floor, ponto fixo) | 416 execuções, 0 falhas (Unicorn = recompilado = nativo) |
| `insn_diff` sementes 11–12 (regressão do conjunto inteiro) | 59.900 casos, 0 falhas |
| `pytest` | 34 testes passam |

**Cobertura no jogo (1.0.0)**, `dkrecomp.coverage`:

| Módulo | Funções 100% suportadas | Instruções suportadas |
|---|---|---|
| `main` | 90,8% → **99,5%** (57.489 / 57.776) | 97,32% → **99,89%** |
| `sdk` | 97,2% → 99,3% | 98,74% → 99,65% |
| `subsdk0` | 97,9% → 99,9% | 99,76% → 99,997% |

O que resta no `main` (287 funções): SIMD vetorial — `fmul`/`fadd` em vetores, `frecpe`/`frecps`/`frsqrte`/`frsqrts`,
`dup`/`mov` de elemento, `sshll`, `tbl`, `ld1`/`st1`, `zip`, `ext`, comparações vetoriais.

Bug encontrado pelo teste de função: nenhum no lifter; o Clang usa conversões em ponto fixo (`ucvtf s0, w8, #6`)
para divisões por potência de dois, que foram então implementadas.

## 2026-10-30 (tarde) — ExeFS 1.0.0 analisada (MVP 2 concluído)

O usuário extraiu a ExeFS e o `control.nacp` do jogo base com as próprias ferramentas e chaves, em
`SwitchExtract/` e `user_files/` (ambas ignoradas pelo Git). Relatórios em `reports/identify_exefs_v0.*` e
`reports/coverage_v0.*` (ignorados pelo Git).

| Resultado | Evidência |
|---|---|
| Base = versão 1.0.0 | `control.nacp` |
| ExeFS íntegra: hashes dos segmentos de `main`, `sdk`, `subsdk0`, `rtld` conferem | `dkrecomp.identify user_files/exefs` |
| SDK 4.4.0, NVN 4.4.0, motor da Retro Studios, Scaleform, NEX 4.2.1 | strings e símbolos |
| `main` tem 57.776 funções com nome e tamanho (96,3% do código) | `.dynsym` |
| Decodificador aceita 97,32% das instruções do `main`; 90,8% das funções inteiras | `dkrecomp.coverage user_files/exefs` |
| O que falta é quase todo FP escalar; no `sdk` também AES/SHA, `CNTPCT_EL0`, `CTR_EL0`, CRC32 | idem |

Marcos: **MVP 2 concluído** (2a NSPs, 2b ExeFS, 2c cobertura). Nova ferramenta: `tools/dkrecomp/coverage.py`.

## 2026-10-30 — Fase 1 a 4 iniciais

### Estado dos marcos

| Marco | Estado | Evidência |
|---|---|---|
| MVP 1 — Ferramentas | **Concluído** | `pytest`: 29 testes passam; `ctest`: `dkr_selftest` passa (build CMake + Ninja + Clang) |
| MVP 2 — Análise | **Parcial** (2a concluído; 2b/2c bloqueados) | `identify --hash` nos dois NSPs: título, versão e SHA-256 de 12/12 conteúdos conferem |
| MVP 3 — Recompilação | **Concluído para o subconjunto suportado** | `func_diff`: funções ARM64 recompiladas = Unicorn = C nativo, em O0/O1/O2/Os; `insn_diff` sem falhas |
| MVP 4 — Integração | **Concluído (escopo de teste)** | `func_diff`: despacho indireto pela tabela do runtime, atômicos via `rt_cas`, `SVC #0x26` entregue ao `dkr_svc_hook` e retomado, memória mapeada por `rt_map_fixed` |
| MVP 5 — Primeiro boot | Não iniciado | Bloqueado por B1 (ExeFS) e pela falta de aritmética SIMD/FP |
| MVP 6 — Gameplay | Não iniciado | — |

### Identificação dos arquivos do usuário (MVP 2a)

`dkrecomp.identify "game original meu" --hash`:

| NSP | Tipo | Title ID | Versão | Conteúdos | SHA-256 |
|---|---|---|---|---|---|
| `[0100C1F0051B6000][v0].nsp` (6,6 GiB) | Application | `0100c1f0051b6000` | 0 | Program, Control, LegalInformation, Meta | 4/4 conferem |
| `[0100C1F0051B6800][v131072].nsp` (43,2 MiB) | Patch | `0100c1f0051b6800` | 131072 (2ª atualização) | 4 DeltaFragment, Program, Control, LegalInformation, Meta | 8/8 conferem |

Tickets: *common*, key generation 4, rights id = title id. Valores registrados em `titles/dkctf/title.toml`.

### Recompilador

Operações suportadas (decodificador + lifter), todas cobertas pelos testes diferenciais:

- Inteiros: ADD/SUB/ADDS/SUBS (imediato, registrador deslocado, estendido), ADC/SBC(S), AND/ORR/EOR/ANDS
  (imediato e registrador, com inversão BIC/ORN/EON/BICS), MOVZ/MOVN/MOVK, ADR/ADRP, SBFM/BFM/UBFM, EXTR,
  MADD/MSUB, SMADDL/UMADDL/SMSUBL/UMSUBL, SMULH/UMULH, UDIV/SDIV, LSLV/LSRV/ASRV/RORV, CSEL/CSINC/CSINV/CSNEG,
  CCMP/CCMN, CLZ/CLS/RBIT/REV/REV16/REV32.
- Desvios: B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, BR, BLR, RET; SVC e BRK (para o runtime).
- Memória: LDR/STR de 8–64 bits com e sem sinal (offset, pré/pós-indexado, sem escala, registrador estendido),
  LDP/STP/LDPSW, LDR literal, LDAR/STLR, LDXR/LDAXR/STXR/STLXR, PRFM (ignorado), DMB/DSB/ISB, CLREX.
- Sistema: MRS/MSR de NZCV, TPIDR_EL0, TPIDRRO_EL0 (leitura), FPCR, FPSR; MRS de CNTVCT/CNTFRQ/DCZID.
- SIMD/FP (só movimentação): LDR/STR/LDP/STP de B/H/S/D/Q (todas as formas de endereço), LDR literal, FMOV entre
  registradores gerais e SIMD (W↔S, X↔D, X↔V.D[1]), INS (geral), UMOV, DUP (geral e escalar), MOVI 64-bit,
  AND/BIC/ORR/ORN/EOR vetoriais.

### Testes diferenciais

| Teste | Volume | Resultado |
|---|---|---|
| `insn_diff` sementes 1–4 (antes do SIMD), 8.000 instruções × 3 estados | 95.857 casos | 0 falhas |
| `insn_diff` sementes 11–14 (com SIMD), 10.000 instruções × 3 estados | 119.751 casos, 68 tipos de operação | 0 falhas |
| `func_diff` O0/O1/O2/Os, 8 sementes × 12 testes (inclui SVC) | 384 execuções | 0 falhas (Unicorn = recompilado = nativo) |
| `pytest` (unitários + diferenciais reduzidos) | 29 testes | 0 falhas |
| `ctest` (`dkr_selftest`, valores conhecidos do `cpu.h`) | 1 teste, ~40 verificações | 0 falhas |
| Teste de mutação (2 bugs plantados) | — | ambos detectados |

### Bugs encontrados pelos testes e corrigidos

1. **LDR literal lia o endereço errado**: o campo do endereço literal se chamava `addr` e colidia com o endereço da
   instrução. Detectado por `insn_diff` (2 casos); renomeado para `lit`.
2. **LDAR/STLR com `o0=0` aceitos**: são LDLAR/STLLR (ARMv8.1), inexistentes no Switch. O Unicorn (A72) os rejeita;
   o decodificador agora também.
3. **Sobreposições CONSTRAINED UNPREDICTABLE aceitas** (ex.: `ldp x0, x0`): detectadas comparando com o Capstone;
   agora rejeitadas.
4. **Mapeamento fixo falhava fora de 64 KiB** e **commit atravessando reservas** (erro 487): corrigidos em
   `rt_map_fixed`.
5. Falha de teste que **não era** bug do lifter: o monitor exclusivo do Unicorn vazava entre casos (1 em ~60 mil).
   Confirmado executando `CLREX` antes de cada caso.
6. **Laço infinito no modo de teste passo-a-passo**: um desvio para a própria instrução (`b .`) virava `goto` para
   si mesmo, enquanto o Unicorn executa um passo. Corrigido no modo `pc` do lifter; o runner agora tem tempo-limite.
   O modo normal de recompilação não era afetado.

### Ambiente

Ferramentas instaladas só no `.venv` do projeto (`requirements-dev.txt`). Nada global foi alterado. Referências em
`refs/` (somente leitura, ignoradas pelo Git).

### Próximo passo

1. Usuário fornece a ExeFS (e o `control.nacp`) extraídos da versão alvo → rodar `identify` na ExeFS e medir a
   cobertura do decodificador sobre o `main` real (MVP 2b/2c).
2. Aritmética SIMD/FP no lifter, com testes diferenciais (decisão de reutilizar ou não o código do TotK/LAS).
3. Descoberta de funções em NSO sem símbolos e loader NSO com relocações (pré-requisitos do MVP 5).
