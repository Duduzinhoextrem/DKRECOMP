# Estudo de viabilidade — DKRecomp

Data: 2026-10-30. Este documento separa **fatos comprovados** (com a evidência), **hipóteses** (plausíveis, ainda
não verificadas) e **incógnitas** (sem base para afirmar nada). Ele deve ser atualizado sempre que uma hipótese for
confirmada ou derrubada.

## 1. Resumo

A recompilação estática de jogos de Switch para Windows x86-64 **é comprovadamente possível**: dois projetos públicos
(LAS Recomp, *Link's Awakening*; TotK Recomp, *Tears of the Kingdom*) rodam jogos comerciais completos assim. Isso
não prova que *Tropical Freeze* funcionará: cada jogo usa versões diferentes do SDK, da API gráfica (NVN), de
serviços do sistema e de middleware, e boa parte do trabalho desses projetos é específica de cada jogo.

O que já está demonstrado **no DKRecomp**, com testes reproduzíveis e sem arquivos do jogo:

- um decodificador ARM64 e um recompilador ARM64 → C próprios, para inteiros e para movimentação SIMD/FP;
- funções inteiras compiladas para ARM64 pelo Clang, recompiladas e executadas no Windows, com resultado idêntico
  ao do Unicorn (CPU ARM64 de referência) e ao do mesmo C compilado nativamente (ver `docs/TESTING.md`);
- identificação e verificação de integridade dos NSPs do usuário, sem decriptação.

**Atualização 2026-10-30 (ExeFS 1.0.0 extraída pelo usuário):** o `main` exporta nome e tamanho de 57.776
funções (96,3% do código), o decodificador já aceita **97,3% das instruções do `main`** e **90,8% das funções**
inteiras, e o que falta é quase todo ponto flutuante escalar. O jogo usa NVN 4.4.0 e o motor próprio da Retro
Studios. Detalhes na seção 2.5.

## 2. Fatos comprovados

### 2.1 Arquivos do usuário (lidos por `dkrecomp.identify --hash`)

| Fato | Evidência |
|---|---|
| Jogo base: Application `0100C1F0051B6000`, versão 0 | `.cnmt.xml` do NSP base; prefixo do rights id do ticket confere |
| Atualização: Patch `0100C1F0051B6800`, versão 131072 (2ª atualização, `131072 = 2 << 16`), `OriginalId` = base | `.cnmt.xml` do NSP de update |
| Os 12 conteúdos dos dois NSPs estão íntegros | SHA-256 de cada NCA calculado = SHA-256 listado no meta |
| NCA de programa do base: 7.131.660.288 bytes; do update: 21.430.272 bytes | meta + tamanho no contêiner |
| Update contém 4 `DeltaFragment` além de Program/Control/LegalInformation/Meta | meta do update |
| Key generation 4; RequiredSystemVersion do base = 4.1.0 | meta e ticket |
| Tickets são do tipo *common* (não personalizados) | campo title key type do ticket |
| Executáveis e dados estão em NCAs criptografados | formato NCA; o DKRecomp não lê seu conteúdo |

### 2.2 Projetos de referência (lidos em `refs/`, somente leitura)

| Projeto | Licença | O que é |
|---|---|---|
| totk-pc-recomp (c3b9a4e) | MIT | Port completo de TotK: lifter Python ARM64→C, runtime C, NVN→Vulkan, tradutor de shaders do Ryujinx (.NET) |
| LASRecomp (a6b4f03) | MIT | Port de *Link's Awakening*; origem do lifter, do loader NSO e dos testes diferenciais do TotK |
| SwitchRecomp (0f99e1c) | MIT OR Apache-2.0 (Cargo.toml; sem arquivo LICENSE na raiz) | Especificações e esqueleto em Rust; não roda jogos |
| N64Recomp (ffb39cd) | MIT | Recompilador MIPS→C do N64; referência de desenho (uma função C por função, tabela para chamadas indiretas) |

Como o TotK Recomp funciona (de `docs/architecture.md` e do código):

1. **Entrada**: NSP/XCI + chaves do console do usuário; extração com hactool (com patches para BKTR/update).
   Versões reconhecidas pelo SHA-256 do `main`.
2. **CPU**: `recomp/lifter.py` decodifica ARM64 e emite **uma função C por função guest**; `BL` vira chamada C
   direta; desvios indiretos passam por `rt_dispatch`, que busca o alvo numa tabela de endereços. Flags NZCV e
   registradores mais usados ficam em variáveis locais. SIMD/FP é rebaixado para C (`simd_c.py`, 1.157 linhas).
   Tabelas de salto são resolvidas por análise simbólica própria (`jumptables.py`).
3. **Memória**: endereço guest = endereço host, tudo abaixo de 2^39; regiões reservadas com `MEM_WRITE_WATCH`
   para a GPU saber quais páginas a CPU escreveu.
4. **Sistema**: fronteira no nível das funções `nn::*` do SDK (substituídas por símbolo), poucos serviços IPC reais
   (`audren:u`, `sm:`), SVCs implementados no host. Threads guest = threads host, com um modelo de 3 núcleos
   virtuais e preempção por prioridade imitando o escalonador do Horizon (o jogo depende disso).
5. **Gráficos**: front-end NVN (registra comandos) + back-end Vulkan 1.3 que reproduz os comandos;
   shaders Maxwell traduzidos para SPIR-V pelo tradutor do Ryujinx 1.3.2 (processo separado, .NET NativeAOT);
   pipelines com `VK_EXT_graphics_pipeline_library`; requer `VK_EXT_external_memory_host`.
6. **Toolchain**: Clang + LLD (MSYS2) — o C gerado usa extensões GNU (`__int128`, `musttail`).
7. **Testes**: cada classe de instrução comparada com o Unicorn; funções inteiras comparadas também.

### 2.3 Ambiente desta máquina (detectado)

| Item | Valor |
|---|---|
| CPU | AMD Ryzen 7 5700X, 8 núcleos / 16 threads; AVX2, FMA e BMI2 presentes (lidos por CPUID) |
| RAM | 31,9 GiB |
| GPU | NVIDIA GeForce RTX 5060 Ti, driver 621.1 (Vulkan), `apiVersion` 1.4.357 |
| Python | 3.13.14 (venv do projeto em `.venv`) |
| Visual Studio | 2026 Community 18.11, MSVC 14.51 — **sem Windows SDK** (não liga programas) |
| Clang | via pacote `ziglang` 0.16.0 no venv (Clang + LLD + MinGW-w64), alvo x86_64 e aarch64 |
| CMake / Ninja | 4.4.4 / 1.13.2, no venv |
| Git | 2.53 |

### 2.4 DKRecomp (este repositório)

Ver `docs/PROGRESS.md` e `docs/TESTING.md` para os números. Resumo: ~215 mil casos de instrução isolada (68 tipos
de operação) e 384 execuções de funções inteiras (O0/O1/O2/Os) concordam com o Unicorn e com o C nativo; um teste de
mutação mostrou que os testes detectam bugs plantados no lifter.

### 2.5 ExeFS do jogo base 1.0.0 (lida por `dkrecomp.identify` e `dkrecomp.coverage`)

Extraída pelo usuário da própria cópia; hashes de todos os segmentos dos 4 NSOs conferem.

| Fato | Evidência |
|---|---|
| Versão exibida do base: 1.0.0 | `control.nacp` (Control NCA `bc3cfae9…`) |
| Módulos: `rtld`, `main` (text 11,6 MiB), `sdk` (4,1 MiB), `subsdk0` (3,0 MiB) | cabeçalhos NSO |
| SDK 4.4.0 (`NintendoSdk_nnSdk-4_4_0`), NVN 4.4.0 no `sdk` | strings de middleware |
| O jogo usa NVN: `main` e `subsdk0` importam `nvnBootstrapLoader`; classes `CGraphicsDevice_NVN`, `CShaderCore_NVN` | símbolos dinâmicos |
| Motor próprio da Retro Studios (`rstl`, `C…GOC`, `CActorModule…`), interface em Scaleform (17.577 funções), rede NEX 4.2.1, libcurl, vídeo pelo SDK `movie` (código de mídia do Android no `subsdk0`) | símbolos e strings |
| Sem Wwise, Havok, Bink ou CRI | nenhum namespace correspondente |
| `main` exporta 65.089 símbolos de função com tamanho (57.776 endereços distintos), cobrindo 96,3% do `.text` | `.dynsym` |
| `sdk` e `subsdk0` exportam funções cobrindo 67% e 64% do `.text`; o resto precisa de descoberta de funções | `.dynsym` |
| Serviços que o jogo pode usar (NPDM): `acc:u0`, `aoc:u`, `apm`, `appletOE`, `audin:u`, `audout:u`, `audren:u`, `bcat:u`, `bsd:u`, `caps:su`, `csrng`, `friend:u`, `fsp-srv`, `hid`, `hwopus`, `irs`, `ldn:u`, `lm`, `mii:u`, `nfp:user`, `nifm:u`, `nvdrv`, `pctl`, `pl:u`, `prepo:u`, `set`, `ssl`, `time:u`, `vi:u` e outros | `main.npdm` |
| Nenhuma instrução `SVC` no `main` nem no `subsdk0`: todas as 57 estão no `sdk` (+4 no `rtld`) | varredura do `.text` |

Cobertura do decodificador (palavras de instrução dentro das funções exportadas):

| Módulo | Funções 100% suportadas | Instruções suportadas | O que falta |
|---|---|---|---|
| `main` | 52.468 / 57.776 (90,8%) | 2.845.908 / 2.924.278 (97,32%) | FP escalar: `fmul` 22k, `fadd` 13k, `fcmp` 11k, `fsub` 11k, `fmov`, `fcsel`, `fdiv`, `fcvt`, `scvtf`…; pouco SIMD vetorial |
| `sdk` | 15.955 / 16.408 (97,2%) | 98,74% | FP escalar; AES/SHA em hardware; `CNTPCT_EL0`, `CTR_EL0`; CRC32 |
| `subsdk0` | 7.919 / 8.091 (97,9%) | 99,76% | FP escalar |

Hipóteses resolvidas: H2 (NVN) **confirmada**; H3 confirmada, com a surpresa de que o `main` tem símbolos;
H4 confirmada (SDK 4.4.0); H6 **parcialmente**: o jogo usa muito FP, mas quase todo escalar; H7 confirmada
(motor próprio + Scaleform). H1 (v131072 = 1.0.2) continua aberta.

## 3. Comparação: TotK Recomp × necessidades do DKRecomp

| Componente do TotK | Genérico ou específico? | Situação para Tropical Freeze |
|---|---|---|
| Lifter ARM64→C (`lifter.py`, `gen.py`, `flags.py`, `regs.py`) | Genérico | Reutilizável em princípio (MIT). O DKRecomp tem lifter próprio menor; decisão pendente (seção 6) |
| Rebaixamento SIMD/FP (`simd_c.py`, `simd.h`, `simd_fp.h`) | Genérico | O maior pedaço que falta ao DKRecomp; candidato forte a reutilização com atribuição |
| Resolução de tabelas de salto (`jumptables.py`) | Genérico (padrões do Clang) | Útil; o jogo foi compilado com outra versão do compilador da Nintendo — padrões podem diferir |
| Loader NSO, MOD0, relocações, layout dos módulos | Genérico | Reutilizável; DKRecomp já tem leitor NSO próprio |
| Memória guest=host + write watch (`memory.c`) | Genérico (Windows) | Reutilizável |
| Kernel: threads, núcleos virtuais, preempção, spin gates (`kernel.c`) | Genérico do Horizon, ajustado ao TotK | Modelo reutilizável; listas de spin-waits são por binário e precisam ser recalculadas |
| SVCs, IPC, `sm:`, `audren:u` | Genérico do Horizon, dependente da versão do SDK | Precisa ser checado contra o SDK do DKC TF (mais antigo: RequiredSystemVersion 4.1.0 vs SDK 20.x do TotK 1.4.3) |
| Substituição de funções `nn::*` por símbolo | Genérico na técnica | A lista de funções e seus ABIs dependem da versão do SDK; endereços são por binário |
| Front-end NVN + back-end Vulkan (`nvn_*`, `vk_*`) | Genérico de NVN | Reutilizável se o jogo usar NVN (hipótese H2) e se os layouts de estrutura do NVN da versão do SDK do jogo baterem |
| Tradutor de shaders (Ryujinx 1.3.2, .NET) | Genérico de Maxwell | Reutilizável (MIT), mas é uma dependência pesada (.NET 10 SDK) |
| Texturas: deswizzle block-linear, ASTC | Genérico | Reutilizável |
| Patches de frame-rate/resolução, heaps, zstd, filmes VP9, lista de shaders | **Específico de Zelda** | Não serve; DKC TF precisa dos seus, descobertos por análise própria |
| Hashes esperados, tabelas de versão, endereços | **Específico de Zelda** | Não serve |
| Instalador, menu ImGui, overlay | Genérico | Fora de escopo até haver gameplay |

## 4. Hipóteses (precisam de verificação)

| # | Hipótese | Como verificar | Bloqueio |
|---|---|---|---|
| H1 | A versão 131072 corresponde ao "1.0.2" | Ler `display_version` do `control.nacp` | NCA criptografado; precisa do NACP extraído pelo usuário |
| H2 | O jogo usa NVN como API gráfica | Imports `nvn*`/`nvnBootstrapLoader` no `main`; strings de middleware | ExeFS extraída |
| H3 | O executável tem módulos `rtld`, `main`, `sdk` e possivelmente `subsdk*`, sem símbolos internos do jogo (só `.dynsym`) | `dkrecomp.identify` na ExeFS | ExeFS extraída |
| H4 | O SDK é de uma geração bem mais antiga que a do TotK (RequiredSystemVersion 4.1.0) e por isso os ABIs NVN/serviços diferem | Strings `SDK MW+Nintendo+NintendoSDK_nnSdk-*` no `sdk` | ExeFS extraída |
| H5 | A atualização substitui o `main` (ExeFS nova) e corrige a RomFS por BKTR | Conteúdo do NCA de programa do update (21 MB) | Decriptação (fora do DKRecomp) |
| H6 | O jogo usa SIMD/FP extensivamente (matemática 3D, áudio) | Contagem de instruções não suportadas no `main` | ExeFS extraída |
| H7 | O motor é próprio da Retro Studios, sem Havok/Unreal/Unity | Strings de middleware | ExeFS extraída |
| H8 | O jogo depende do escalonador do Horizon (núcleos fixos, prioridades) como o TotK | Execução | MVP 5 |

## 5. Incógnitas

- Tamanho do `main`, número de funções e quantidade de instruções que o lifter ainda não cobre.
- Quais serviços IPC e SVCs o jogo usa de fato (o NPDM mostra o que ele *pode* usar).
- Formatos de arquivos da RomFS (pacotes `.pak` da Retro são esperados, mas não verificados) — só importam para
  ferramentas de mod ou depuração; o jogo recompilado lê seus próprios arquivos.
- Comportamentos de GPU específicos do jogo (efeitos, pós-processamento) e quantos shaders existem.
- Desempenho: nenhuma medição possível antes de um boot. **Não há promessa de 60 FPS ou resoluções maiores.**

## 6. Decisões de engenharia

1. **Lifter próprio, pequeno e verificado, em vez de copiar o do TotK agora.** Motivo: o pedido exige um projeto
   independente e progresso verificável; um lifter próprio com testes diferenciais dá controle total da semântica.
   Consequência: o DKRecomp ainda não cobre aritmética SIMD/FP, que o TotK já cobre. **Decisão pendente** para a
   Fase 4: reutilizar `simd_c.py`/`simd.h` (MIT, com atribuição em `THIRD_PARTY_NOTICES.md`) sob os nossos testes
   diferenciais, ou escrever o nosso.
2. **C como saída intermediária**, como LAS/TotK/N64Recomp: compila em qualquer compilador C e é depurável.
3. **C gerado compatível com MSVC e Clang** (sem `__int128` obrigatório). Hoje só o Clang foi exercitado de fato,
   porque falta o Windows SDK (ver `docs/KNOWN_ISSUES.md`).
4. **Unicorn só nos testes**, nunca em binários distribuídos (GPL-2.0).
5. **Sem decriptação** no DKRecomp (`docs/LEGAL.md`).
6. **Python para ferramentas, C para runtime.** Rust não traz vantagem que justifique hoje: o C gerado precisa de um
   runtime C, e as referências são C.

## 7. Conclusão (atualizada com a ExeFS)

A frente de CPU ficou bem menor do que se temia: com ponto flutuante escalar, cripto em hardware e alguns
registradores de sistema, o decodificador deve cobrir praticamente todo o código, e os símbolos do `main` dispensam a
descoberta de funções no módulo maior. Os riscos principais passam a ser a camada de sistema do SDK 4.4.0 e,
sobretudo, os gráficos (NVN 4.4.0 → Vulkan, shaders Maxwell, Scaleform).

## 7a. Conclusão original (antes da ExeFS)

Viável como projeto de engenharia de longo prazo, com risco alto e concentrado em três frentes: (1) cobertura
SIMD/FP e escala do lifter para um `main` comercial; (2) camada de sistema (SDK antigo, NVN) diferente da do TotK;
(3) gráficos. Nenhuma dessas frentes pode ser avaliada com precisão antes da ExeFS extraída. A próxima evidência
decisiva é o relatório do `dkrecomp.identify` sobre a ExeFS: ele dirá quantas instruções faltam, quais serviços
são usados e se o jogo usa NVN.
