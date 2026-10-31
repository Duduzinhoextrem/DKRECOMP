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

## IPC e serviços

- `runtime/src/ipc.c` lê a mensagem do TLR (ou do buffer do usuário em `SendSyncRequestWithUserBuffer`), despacha
  para o serviço da sessão (ou para o objeto do domínio) e escreve a resposta no mesmo buffer, como o kernel faz.
- `runtime/src/services.c` registra os serviços por nome; `sm:GetService` de um nome desconhecido entrega uma
  sessão que registra o primeiro comando e para. Interfaces devolvidas viram handle numa sessão comum ou objeto
  num domínio (`hos_reply_interface`).
- Uma trava do kernel serializa as SVCs, inclusive o tratamento de IPC. As sessões clonadas de um domínio
  compartilham sua tabela de objetos, liberada quando a última sessão é fechada.

## Threads e sincronização

- `runtime/src/kernel.c` associa uma thread do Windows a cada thread guest, com `Cpu`, pilha guest e TLR
  próprios. A pilha C do host tem 8 MiB nas threads criadas e 16 MiB na thread principal.
- Cada thread bloqueada espera em uma condição do Windows que libera a trava do kernel. A liberação de um
  mutex entrega sua palavra ao waiter de maior prioridade; empates seguem a ordem de chegada. Uma condição
  sinalizada primeiro re-adquire o mutex, sem continuar aplicando o timeout da espera pela condição.
- Eventos permanecem sinalizados até serem limpos. Threads encerradas são objetos sinalizados. O vigia informa
  a última SVC e a espera de cada thread. O escalonamento continua sendo do Windows; núcleos e prioridades do
  Horizon não têm preempção própria nem herança de prioridade.

## RomFS

- `tools/dkrecomp/romfs.py` converte uma pasta já extraída em uma imagem RomFS crua: header de 0x50 bytes,
  dados a partir de 0x200, arquivos alinhados a 16 bytes e tabelas de diretórios, arquivos e hashes ao final.
  Usa streaming e publica a saída somente depois de terminar. Rejeita links e saída dentro da pasta de entrada.
- `runtime/src/fs.c` implementa `fsp-srv:OpenDataStorageByCurrentProcess` e `IStorage:Read/GetSize/Flush`.
  Lê a imagem no disco por offsets de 64 bits; o próprio SDK guest procura os arquivos nas tabelas RomFS.
- A imagem padrão é `user_files/romfs.bin`, relativa ao layout gerado. `DKR_ROMFS` ou o terceiro argumento do
  executável seleciona outra imagem. Nenhuma imagem ou arquivo do jogo faz parte do código versionado.
- Formato conferido com os [tipos de RomFS do libnx](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/runtime/devices/romfs_dev.h),
  o [leitor do libnx](https://github.com/switchbrew/libnx/blob/master/nx/source/runtime/devices/romfs_dev.c) e o parser local `nstool`.

## Organização do repositório

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
| `runtime/src/horizon.c`, `kernel.c`, `ipc.c`, `services.c`, `fs.c` | Boot, SVCs, threads, sincronização, IPC, serviços e RomFS | Parcial, guiado pelo boot |
| `runtime/gfx/` | NVN → Vulkan | *(planejado)* |
| `runtime/audio/`, `runtime/input/`, `runtime/config/` | Áudio (SDL3), entrada, configuração | *(planejado)* |
| `tests/python/` | Testes unitários (formatos, decodificador × Capstone) | Pronto |
| `tests/diff/` | Testes diferenciais: instrução isolada e funções inteiras | Pronto |
| `tests/native/` | Testes de valor conhecido do `cpu.h`, via CMake | Pronto |
| `tools/toolchain/` | Wrappers e toolchain CMake do Clang do Zig | Pronto |

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
