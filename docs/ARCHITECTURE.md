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
| `runtime/services/` | SVCs, IPC, serviços do Horizon | *(planejado)* |
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
