# Arquitetura

Como o DKRecomp transforma o executável ARM64 do Switch num programa x86-64 para Windows, e como o código está
organizado. Partes marcadas *(planejado)* ainda não existem.

## Fluxo

```
NSP do usuário ──► identify (PFS0, ticket, CNMT, SHA-256)          [ferramentas, sem decriptação]
ExeFS extraída ──► identify (NSO, NPDM, imports, SVCs, middleware)
                     │
main/sdk NSO ──► decoder ARM64 ──► lifter ──► C gerado ──► Clang ──► exe
                                      │                       ▲
                                      └── relatório de        │
                                          instruções não      runtime (C): CPU, despacho, memória,
                                          suportadas           SVC/serviços*, gráficos*, áudio*, entrada*
```

`*` planejado.

## Modelo de execução

- **Uma função C por função guest** (`f_<endereço>(Cpu *c)`), como LAS Recomp, TotK Recomp e N64Recomp.
- **Estado da CPU** em `struct Cpu` (`runtime/include/dkr/cpu.h`): X0–X30, SP, NZCV (bits 31..28, o formato do
  `MRS NZCV`), TPIDR/TPIDRRO, FPCR/FPSR, monitor exclusivo e V0–V31 (128 bits).
- **Memória**: endereço guest = endereço host. O runtime mapeia a memória guest no mesmo endereço numérico
  (`rt_map_fixed`, reservas de 64 KiB do Windows), então loads/stores viram acessos de ponteiro.
- **Desvios**:
  - dentro da função → `goto L_<addr>`;
  - `BL` para função conhecida → chamada C direta, com X30 = endereço de retorno; `RET` → `return`
    (premissa: código de compilador retorna para o LR que o BL deixou — a mesma dos projetos de referência);
  - `B` para fora da função → chamada em posição de cauda;
  - `BLR`/`BR` e alvos desconhecidos → `rt_dispatch`, busca binária na tabela `dkr_fn_table` gerada;
  - `BR` com alvo possivelmente interno (tabelas de salto) → `switch (c->pc)` local sobre todos os endereços da
    função; alvos fora caem no `rt_dispatch`. (Resolução estática de tabelas de salto está no roadmap.)
- **Semântica que difere entre C/x86 e A64** fica em funções auxiliares testadas: divisão (A64 não gera exceção em
  x/0 nem em INT_MIN/-1), deslocamentos módulo a largura, flags de `AddWithCarry`, `CLS`, `RBIT`, `REV*`,
  multiplicação alta de 128 bits (portável MSVC/Clang).
- **Ponto flutuante** (`runtime/include/dkr/fp.h`): valores trafegam como bits; o host calcula com SSE (IEEE 754),
  e os auxiliares corrigem onde A64 e x86 diferem: escolha do NaN propagado (SNaN primeiro, na ordem dos operandos),
  NaN padrão positivo para operações inválidas, conversões para inteiro com saturação e arredondamento explícito,
  FMA fundido, `max/min` com ±0. Escritas escalares zeram o resto do registrador V. Só FPCR = 0 é modelado.
- **SIMD vetorial** (`runtime/include/dkr/simd.h`): cada auxiliar recebe registradores inteiros por valor e
  devolve o resultado, então `Vd == Vn` não causa problema; Q=0 zera os 64 bits altos. As operações FP por lane
  reusam os auxiliares escalares de `fp.h`, com a mesma semântica de NaN. As estimativas FRECPE/FRSQRTE seguem o
  algoritmo da Arm (verificado bit a bit contra o Unicorn).
- **Atômicos**: `LDXR/STXR` emulados com monitor por CPU e compare-and-swap no store (`rt_cas`); `STLR` seguido de
  barreira completa, porque o x86 pode antecipar um load posterior a um store.
- **Instruções não suportadas** viram `rt_unsupported(c, pc, word)` (aborta com o endereço) e entram no relatório
  do módulo. Nada é "emulado de mentira".

## Carregamento e boot

- **Ligação estática** (`tools/dkrecomp/link.py`): os módulos (`main`, `subsdk*`, `sdk`; o `rtld` não é carregado)
  recebem bases fixas a partir de `0x800000000`, alinhadas a 2 MiB; relocações e símbolos importados são resolvidos
  na geração, e cada módulo vira uma imagem pronta (`<módulo>.img`).
- **Descoberta de funções** (`tools/dkrecomp/gen.py`): símbolos, destinos de desvios, ponteiros relocados,
  endereços materializados por ADR/ADRP+ADD e tabelas de busca de `.eh_frame_hdr`. As tabelas unwind usam os
  endereços relativos ao header (sdata4), incluindo callbacks que não são destinos de BL. `chunks.json`, dentro
  da saída ignorada pelo Git, conserva os limites dos arquivos C entre gerações.
- **Loader** (`runtime/src/loader.c`): mapeia cada módulo na sua base (endereço guest = host), copia a imagem, deixa
  o `.bss` zerado e registra a tabela de despacho do módulo.
- **Boot** (`runtime/src/horizon.c`): cria a pilha e a região local da thread principal (TLR, via `TPIDRRO_EL0`),
  registra os callbacks do `rtld` como funções do host com endereço guest (`rt_hostfn_register`) e chama
  `nn::init::Start` como o `rtld` faz. Cada SVC é registrada no log; o que não está implementado encerra a execução
  com relatório.
- O corpo recompilado de `nnMain` registra sua entrada no log, inclusive se for chamado diretamente pelo SDK.
- Overrides de host são resolvidos pelos endereços dos símbolos, incluindo aliases, e substituem o corpo C
  normal da função. Chamadas diretas e indiretas chegam ao mesmo tratador. O corpo original fica num trampoline
  separado, listado por módulo e acessível explicitamente por `rt_call_original`; isso permite delegar ao SDK
  depois de ajustar argumentos. O manifesto separa esses corpos das funções normais. `--gfx` habilita NVN e áudio CPU.

## IPC e serviços

- `runtime/src/ipc.c` lê a mensagem do TLR (ou do buffer do usuário em `SendSyncRequestWithUserBuffer`), despacha
  para o serviço da sessão (ou para o objeto do domínio) e escreve a resposta no mesmo buffer, como o kernel faz.
- `runtime/src/services.c` registra os serviços por nome; `sm:GetService` de um nome desconhecido entrega uma
  sessão que registra o primeiro comando e para. Interfaces devolvidas viram handle numa sessão comum ou objeto
  num domínio (`hos_reply_interface`).
- Uma trava do kernel serializa as SVCs, inclusive o tratamento de IPC. As sessões clonadas de um domínio
  compartilham sua tabela de objetos, liberada quando a última sessão é fechada.
- Objetos Service contam as referências de sessões e domínios; um destrutor opcional libera os recursos do
  objeto após a última referência. A IFileSystem de save usa esse mecanismo para liberar seu estado.
- O applet manager fornece o usuário local por `PopLaunchParameter(2)`, numa IStorage de 0x88 bytes;
  Open devolve IStorageAccessor, com GetSize e Read. Esse parâmetro é consumido uma vez e usa o mesmo UID de
  `acc:u0`, permitindo que o SDK abra o usuário antes de montar o save.

## Threads e sincronização

- `runtime/src/kernel.c` associa uma thread do Windows a cada thread guest, com `Cpu`, pilha guest e TLR
  próprios. A pilha C do host tem 8 MiB nas threads criadas e 16 MiB na thread principal.
- Cada thread bloqueada espera em uma condição do Windows que libera a trava do kernel. A liberação de um
  mutex entrega sua palavra ao waiter de maior prioridade; empates seguem a ordem de chegada. Uma condição
  sinalizada primeiro re-adquire o mutex, sem continuar aplicando o timeout da espera pela condição.
- Eventos permanecem sinalizados até serem limpos. Threads encerradas são objetos sinalizados. O vigia informa
  a última SVC e a espera de cada thread. O escalonamento continua sendo do Windows; núcleos e prioridades do
  Horizon não têm preempção própria nem herança de prioridade.

## Gráficos do host

- `runtime/include/dkr/gfx.h` define uma API independente de CPU guest; `runtime/gfx/vk.c` cria janela Win32,
  instância, device, fila e swapchain Vulkan. As funções são carregadas do loader de sistema, sem biblioteca
  de importação nem Windows SDK externo. O contexto pede Vulkan 1.1; não há pipeline de desenho NVN ainda.
- Um fence protege o command buffer e os recursos de cada submissão. Cada imagem da swapchain tem seu próprio
  semáforo de conclusão para apresentação. Resize aguarda o device e recria as imagens e recursos de readback.
- Capturas copiam os pixels da imagem da swapchain para memória host-visible, esperam a conclusão e invalidam
  caches se a memória não for coerente. O BMP permite testar o conteúdo que a GPU produziu.
- O build gráfico é ativado com `DKR_BUILD_GFX=ON`. Os headers e a camada de validação opcional ficam na pasta
  local `toolchain/`, com revisão/digest e avisos em `THIRD_PARTY_NOTICES.md`.
- `runtime/gfx/nvn.c` liga o SDK ao host no boot com `--gfx`: registra endereços para nomes públicos NVN antes
  das threads, cria o contexto Vulkan em `nv::InitializeGraphics` e fornece o lookup de procs. Funções ainda
  ausentes param com seu nome. `nvn_obj.c` mantém os objetos e grava comandos; `nvn_tex.c` calcula layouts.
  `nvn_originals.c` gerado fornece 211 rotinas puras do próprio driver recompilado (builders, estados, views),
  selecionadas por `nvnmap.py`. O modelo atual aceita submits, sem replay Vulkan nem shaders; nenhum desenho.

## Memória compartilhada e HID

- `runtime/src/shared.c` mantém sections do Windows, handles com referências e views guest com permissões reais.
  Uma view host permanece gravável para o serviço; aliases guest veem as mesmas páginas sem cópia periódica.
  `VirtualAlloc2` e `MapViewOfFile3` substituem placeholders divididos, permitindo bases guest de 4 KiB.
  SVCs Map/UnmapSharedMemory registram/removem regiões Shared em `QueryMemory`.
- `runtime/src/hid.c` atende CreateAppletResource e GetSharedMemoryHandle com um bloco de 256 KiB, leitura no
  guest. Ativação, configuração/getters de Npad e lista de dispositivos de vibração cobrem o caminho inicial.
  Os rings permanecem vazios e os controles desconectados até existir um back-end de entrada do host.
- Eventos têm objetos referenciados por handles; cópias compartilham sinalização e reset e acordam waiters de
  todos os aliases. O kernel escolhe o primeiro índice sinalizado no array, inclusive para aliases do mesmo evento.
- `runtime/src/vi.c` expõe um display lógico 1280×720 e identidades de layers/Binder. Serviços de aplicação
  guardam seus privilégios e aberturas; interfaces filhas têm vida independente. O NativeWindow é um parcel público
  desserializado pelo próprio SDK. Refcounts, evento de disponibilidade e Connect/Disconnect do produtor são
  rastreados. O parser de parcels confere bounds e token UTF-16, com X/C ou A/B. Dequeue/QueueBuffer ainda não
  existem. O evento de disponibilidade não equivale ao VSync do display. Escala solicitada fica armazenada.
- Protocolos VI/Binder: [VI services](https://switchbrew.org/wiki/VI_services#NativeWindow),
  [Nvnflinger](https://switchbrew.org/wiki/Nvnflinger_services) e
  [produtor de buffers do libnx](https://github.com/switchbrew/libnx/blob/master/nx/source/display/buffer_producer.c).
- Contratos do host: [MapViewOfFile3](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile3)
  e [VirtualFree](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree).
  Protocolo: [HID](https://switchbrew.org/wiki/HID_services#IAppletResource) e
  [SVC](https://switchbrew.org/wiki/SVC#MapSharedMemory). Implementação própria.

## RomFS

- `tools/dkrecomp/romfs.py` converte uma pasta já extraída em uma imagem RomFS crua: header de 0x50 bytes,
  dados a partir de 0x200, arquivos alinhados a 16 bytes e tabelas de diretórios, arquivos e hashes ao final.
  Usa streaming e publica a saída somente depois de terminar. Rejeita links e saída dentro da pasta de entrada.
- `runtime/src/fs.c` implementa `fsp-srv:OpenDataStorageByCurrentProcess` e `IStorage:Read/GetSize/Flush`.
  Lê a imagem no disco por offsets de 64 bits; o próprio SDK guest procura os arquivos nas tabelas RomFS.
- A imagem padrão é `user_files/romfs.bin`, relativa ao layout gerado. `DKR_ROMFS` ou o terceiro argumento do
  executável seleciona outra imagem. Nenhuma imagem ou arquivo do jogo faz parte do código versionado.
- `runtime/src/save.c` monta saves de conta em `user_files/saves/<program-id>/<uid>/` (ou `DKR_SAVE_ROOT`).
  Normaliza a raiz absoluta, cria seus componentes e recusa reparse points. IFileSystem/IFile implementam consulta,
  criação/remoção não recursiva, abertura, leitura/escrita, tamanho, flush e commit dos arquivos abertos. Paths
  absolutos do guest ficam relativos à raiz; componentes perigosos e nomes de dispositivo Win32 são recusados.
  O estado do filesystem é retido pelos arquivos abertos, independentemente do tempo de vida da sessão IPC.
- Formato conferido com os [tipos de RomFS do libnx](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/runtime/devices/romfs_dev.h),
  o [leitor do libnx](https://github.com/switchbrew/libnx/blob/master/nx/source/runtime/devices/romfs_dev.c) e o parser local `nstool`.

## Organização do repositório

O serviço `audren:u` começa em `runtime/src/audio.c`, com dimensionamento do work buffer REV1–4 sobre o
parâmetro interno de 0x34 bytes. A fórmula DSP é adaptada do nxnative MIT (origem/licença nos avisos de terceiros).
No modo `--gfx`, `audio_sdk.c` converte o parâmetro público para CPU/manual e delega ao servidor/processador
recompilados do SDK 4.4, incluindo parsing de updates, vozes e efeitos. O host preserva o orçamento DSP e aloca
armazenamento adicional em regiões guest visíveis a QueryMemory. A thread é criada pelo SDK e usa sua própria
ThreadType/TLR. O evento é de kernel, com dois handles compartilhando a mesma identidade.

O hook de execução serializa geração, processamento e captura com o mutex de Update do SDK. `audio_pcm.c`
extrai o DeviceSink SDK 4 e converte para PCM estéreo de 16 bits. WinMM devolve os buffers tocados para cadenciar
a thread. Sem dispositivo ou com `DKR_AUDIO_NULL=1`, o renderer usa um temporizador e registra a ausência de som.
O boot comprovou abertura e processamento de quadros silenciosos; ainda não comprovou áudio audível do jogo.

| Pasta | Conteúdo | Estado |
|---|---|---|
| `tools/dkrecomp/formats/` | Leitores: PFS0 (NSP), ticket (campos públicos), CNMT XML, NSO (com LZ4), NPDM, NACP, ELF | Pronto, testado com dados sintéticos |
| `tools/dkrecomp/identify.py` | Identificação e validação de arquivos do usuário; relatório JSON + Markdown | Pronto (NSP verificado com os arquivos reais) |
| `tools/dkrecomp/titles.py`, `titles/<jogo>/title.toml` | Releases conhecidas por título; uma pasta por jogo | DKC TF cadastrado |
| `tools/dkrecomp/arm64/decoder.py` | Decodificador A64 (subconjunto inteiro + movimentação SIMD/FP) | Pronto para o subconjunto |
| `tools/dkrecomp/recomp/lifter.py` | A64 → C, uma instrução por vez, funções inteiras | Pronto para o subconjunto |
| `tools/dkrecomp/recomp/module.py` | Módulo inteiro → um `.c` com protótipos, funções e tabela de despacho | Pronto |
| `tools/dkrecomp/testing/` | Infra de testes diferenciais (Unicorn × harness) e gerador de casos | Pronto |
| `runtime/include/dkr/` | `cpu.h` (estado e auxiliares para o código gerado), `rt.h` (API do host) | Pronto |
| `runtime/src/core.c` | Despacho, CAS, barreiras, mapeamento fixo, contador de sistema, tratadores de trap | Pronto |
| `runtime/src/horizon.c`, `kernel.c`, `ipc.c`, `services.c`, `fs.c`, `save.c`, `shared.c`, `hid.c`, `vi.c` | Boot, SVCs, threads, sincronização, IPC, serviços, RomFS, save, shared memory/HID e display/VI | Parcial, guiado pelo boot |
| `runtime/gfx/` | Janela/apresentação Vulkan próprias verificadas; adaptação NVN pendente | Parcial, independente do jogo |
| `runtime/src/audio*.c` | Dimensionamento IPC, servidor CPU do SDK e saída WinMM | Quadros silenciosos medidos no boot |
| `runtime/input/`, `runtime/config/` | Entrada do host, configuração | *(planejado)* |
| `tests/python/` | Testes unitários (formatos, decodificador × Capstone) | Pronto |
| `tests/diff/` | Testes diferenciais: instrução isolada e funções inteiras | Pronto |
| `tests/native/` | Testes de valor conhecido do `cpu.h`, via CMake | Pronto |
| `tools/toolchain/` | Wrappers e toolchain CMake do Clang do Zig | Pronto |

Os comandos NVN são registros do host com argumentos inteiros/SIMD e payloads imutáveis de dados transitórios
(`nvn_commands.c`). Arrays e estados são copiados durante a chamada; views em SetRenderTargets têm cópia
recursiva de um nível. Call/CopyCommands expandem registros com propriedade compartilhada dos payloads, para
que a invalidação do handle original não destrua uma gravação que o incluiu. Strides acompanham cada registro.
O visitor fornece a fronteira do replay. Endereços GPU continuam apontando aos pools; não são confundidos com
ponteiros para argumentos CPU. O back-end ainda precisa consumir esses registros e controlar recursos/fences reais.

`tools/shader_translator/` fornece o serviço managed Maxwell → SPIR-V, adaptado do TotK/Ryujinx. O build copia
fontes de revisão fixa para `toolchain/`, aplica nove patches e seleciona somente Shader, Spv.Generator e cinco
arquivos de Common. Não há dependência NuGet/NativeAOT. O protocolo JSON permite um processo persistente;
saídas são SPIR-V e reflexão de recursos. A identidade de build acompanha os caches. Três shaders sintéticos
passaram no validador Khronos compilado localmente; o runtime do jogo ainda não está ligado ao serviço.

`dkrecomp.shaderjobs` prepara bibliotecas offline dos programas SNVN em arquivos RFRM MTRL do motor Retro.
Também encontra o material de inicialização LZSS8 dentro de `main`. Valida os comprimentos da forma, chunk,
registro zlib, controles e áreas de código antes de exportar; não aceita conteúdo descompactado excedente.
O tamanho do controle vem de +0x20 (**0x840 no alvo**). O hash SHA-256 inclui controle e código; c1 vem de +0x700
até o fim da área declarada em +0x704. Graphics remove apenas o header NVN de 0x30 e preserva o SPH de 0x50;
compute com magic próprio remove header/padding até +0x100, e headerless permanece intacto. A biblioteca
preserva quais estágios pertencem ao mesmo programa. Os recursos ainda precisam dos metadados de bindings reais.
`testing.shader_library` confere reflexão/estágio e executa o validador Khronos por shader. Bibliotecas e relatórios
contêm dados derivados do jogo e permanecem ignorados pelo Git.

No runtime, `NvnProgramSnapshot` armazena os estágios por cópias de código/controle (`nvn_shaders.c`), incluindo
c1 e dados extras do compilador. O tamanho não é fixado em 0x880. Cada shader recebe a mesma identidade
SHA-256 do preparador Python, via CNG do Windows. Capturas opcionais `.nvnprog` preservam o pareamento do
programa em um formato próprio validado por `shaderjobs --capture`. São publicadas completas por rename;
o caminho precisa ser absoluto e os arquivos nunca são habilitados por padrão. A vida dos recursos Vulkan
e dos programas já referenciados por submissões ainda precisa de integração ao replay.

Os jobs SDK 4.4 usam a faixa de bindings de textura `c[2]+0x20..0x120`, medida no driver original. O serviço
managed continua com seu default NVN 55 para clientes sem os campos `tex_base`/`tex_limit`. Metadados dos
recursos e variantes por draw devem integrar a identidade de cache da tradução, além da identidade do programa.

## Vários jogos

Nada em `tools/dkrecomp/` ou `runtime/` depende de *Tropical Freeze*. O que for específico de um jogo fica em
`titles/<jogo>/` (releases conhecidas hoje; patches, tabelas e listas de funções no futuro). *Donkey Kong Country
Returns HD* entraria como `titles/dkcr_hd/`, com sua própria análise — nenhuma compatibilidade é presumida.

## Toolchain

- **Ferramentas**: Python 3.13 em `.venv` (pytest, unicorn, capstone, ziglang, cmake, ninja, lz4).
- **C gerado e runtime**: Clang do pacote `ziglang` (alvo `x86_64-windows-gnu`), via CMake + Ninja
  (`tools/toolchain/zig-x86_64-windows.cmake`) ou direto pelos scripts de teste.
- **Programas de teste ARM64**: o mesmo Clang com `-target aarch64-freestanding-none`.
- **MSVC**: o código foi escrito para compilar também com MSVC (`DKR_MSVC` em `cpu.h`), mas não pôde ser
  verificado nesta máquina (falta o Windows SDK).
