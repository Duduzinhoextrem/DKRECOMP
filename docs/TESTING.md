# Testes

Nenhum teste precisa dos arquivos do jogo, exceto a identificação dos NSPs do usuário.

## Preparar o ambiente (uma vez)

```bash
python -m venv .venv
.venv/Scripts/python -m pip install -r requirements-dev.txt
```

Todos os pacotes vêm do PyPI e ficam no `.venv` do projeto; nada é instalado no sistema. `ziglang` traz o Clang/LLD
usado para compilar o C gerado (x86-64) e os programas de teste (ARM64).

## Rodar tudo

```bash
.venv/Scripts/python -m pytest
```

Inclui testes unitários (formatos, decodificador × Capstone) e versões reduzidas dos testes diferenciais.
O teste do gerador também compila/executa um módulo sintético para verificar overrides por alias em chamadas
diretas e indiretas, preservando a passagem de argumentos/resultados e registradores não alterados pelo host.

## Testes diferenciais

### Instrução isolada (`tests/diff/insn_diff.py`)

Gera instruções aleatórias a partir de modelos de codificação do manual ARM, mantém as que o decodificador aceita,
gera estados de registradores (valores aleatórios e de borda) e executa cada uma:
- no **Unicorn** (modelo Cortex-A72, ARMv8.0 como o Cortex-A57 do Switch);
- como **C recompilado** (`tests/diff/harness.c`), com a memória mapeada nos mesmos endereços.

Compara X0–X30, SP, NZCV, TPIDR, PC, V0–V31 e os 4 KiB de memória de rascunho.

```bash
.venv/Scripts/python tests/diff/insn_diff.py --seed 11 --count 10000 --states 3
```

Para concentrar os casos numa família, use `--only` com o prefixo dos modelos (por exemplo `--only fp_` para ponto
flutuante escalar). Os casos de FP usam valores de borda (±0, ±∞, NaN silencioso e de sinalização com payload,
subnormais, limites de conversão para inteiro) e operandos relacionados (x−x, ∞−∞, 0×∞).

```bash
.venv/Scripts/python tests/diff/insn_diff.py --seed 31 --count 8000 --states 4 --only fp_
```

`--only vec_` faz o mesmo para o SIMD vetorial (todas as lanes recebem valores de ponto flutuante de borda; índices
de `TBL` caem dentro e logo fora da tabela).

### Funções inteiras (`tests/diff/func_diff.py`)

`tests/diff/cases/programs.c` (recursão, laços, ordenação, bytes com sinal, `switch` com tabela de salto,
multiplicação de 128 bits, ponteiros de função, cópia de structs, atômicos LDAXR/STLXR, seleção condicional, `SVC`
entregue a um tratador do runtime) é
compilado pelo Clang para ARM64. O DKRecomp recompila todas as funções do ELF, e `test_entry(which, seed)` roda de
três formas que precisam concordar: **Unicorn = recompilado = mesmo C compilado nativamente para x86-64**. Recompilado
e Unicorn também precisam concordar em todos os registradores e no `.data/.bss`.

```bash
.venv/Scripts/python tests/diff/func_diff.py --opt O2,O1,O0,Os --seeds 8
```

Por padrão o Clang é chamado com `-fno-vectorize -fno-slp-vectorize`; `--vectorize` liga a vetorização
automática, para que o programa guest tenha SIMD gerado pelo compilador:

```bash
.venv/Scripts/python tests/diff/func_diff.py --vectorize --seeds 8
```

### Teste de mutação (validação dos próprios testes)

Feito manualmente em 2026-10-30 numa cópia do projeto: dois bugs plantados no lifter foram detectados.

| Mutação | `insn_diff` (1.500 instr.) | `func_diff` (O2) |
|---|---|---|
| Carry de `AddWithCarry` errado quando `r == a` | 5 casos falharam | não detectou (caso de borda raro) |
| `CSINC` somando 2 em vez de 1 | 8 casos falharam | 4 execuções falharam |

## Runtime via CMake

```bash
.venv/Scripts/cmake -S . -B build/zig -G Ninja -DCMAKE_MAKE_PROGRAM=.venv/Scripts/ninja.exe -DCMAKE_TOOLCHAIN_FILE=tools/toolchain/zig-x86_64-windows.cmake -DCMAKE_BUILD_TYPE=Release
```

```bash
.venv/Scripts/cmake --build build/zig
```

```bash
.venv/Scripts/ctest --test-dir build/zig --output-on-failure
```

`dkr_selftest` verifica valores conhecidos dos auxiliares do `cpu.h` (divisão, flags, condições, CLZ/CLS, REV,
multiplicação alta).

No Windows, `kernel_sync` usa 6 threads sintéticas para disputar um mutex (12.000 incrementos), condições
(sinal, broadcast, prioridade, timeout e espera pela reaquisição do mutex), eventos e espera pela saída de
threads. `romfs_storage` passa mensagens IPC reais ao `fsp-srv`/`IStorage` usando um arquivo sintético: tamanho,
leituras, fim do arquivo, offsets negativos, overflow e buffer insuficiente. Nenhum desses testes usa o jogo.
Também percorre as interfaces do applet manager para ler o parâmetro de usuário pré-selecionado e compará-lo
ao UID devolvido por `acc:u0`; verifica o consumo único do parâmetro e seus limites de leitura.
Monta um save de conta numa raiz temporária, confere as pastas e remove somente as que o próprio teste criou.
O teste IPC confere a destruição de um objeto após fechar o último clone e o ciclo de um handle de transfer memory.

## Preparar a RomFS já extraída

O gerador recebe uma pasta de arquivos, sem chaves nem NCAs. Recusa sobrescrever a saída sem `--force` e recusa
links ou saída dentro da árvore de entrada. Usa um arquivo temporário e só publica a imagem ao concluir.

```bash
PYTHONPATH=tools .venv/Scripts/python -m dkrecomp.romfs user_files/romfs user_files/romfs.bin
```

Os testes Python verificam hashes, colisões, diretórios, irmãos, UTF-8, arquivos vazios e geração determinística.
O caso de symlink é pulado se o Windows não permitir criá-lo. Na máquina do usuário o parser independente também
foi executado:

```bash
SwitchExtract/nstool.exe -t romfs --fstree user_files/romfs.bin
```

Esse comando não exige chaves para uma imagem crua; o aviso de ausência de keyfile não impede a leitura da RomFS.

## Teste gráfico independente do jogo

Obter os headers Khronos fixados por SHA-256. `--validation` também baixa um snapshot oficial da camada de
validação de teste, confere o digest do asset e registra a revisão em `toolchain/vulkan-validation/source.txt`.
Tudo fica em `toolchain/`, sem instalação ou registro global. O snapshot da camada pode variar; os headers são fixos.

```bash
.venv/Scripts/python tools/fetch_gfx_deps.py --validation
.venv/Scripts/cmake -S . -B build/zig -DDKR_BUILD_GFX=ON
.venv/Scripts/cmake --build build/zig
.venv/Scripts/ctest --test-dir build/zig --output-on-failure
```

Usar a configuração da toolchain Zig descrita acima na primeira criação de `build/zig`. São seis testes no
ambiente com a camada local: quatro anteriores, `vulkan_present` (8 frames) e `vulkan_validation` (32 frames).
O segundo exige a camada e ativa sincronização por `VK_VALIDATION_VALIDATE_SYNC=1`; as variáveis são definidas
somente para esse teste. `DKR_VK_VALIDATION_DIR` pode apontar para outra camada já disponível.

Os dois testes gráficos criam uma janela oculta, apresentam, redimensionam 640×360 → 320×240 e conferem todos
os pixels do BMP lido de volta da GPU. Também incluem no resultado os erros emitidos durante a destruição.
Capturas ficam em `build/zig/gfx-smoke*.bmp`. Para inspeção visível:

```bash
build/zig/dkr_gfx_smoke.exe --capture reports/gfx-visible.bmp --hold-seconds 30
```

Esse teste verifica clear/apresentação/readback; não executa o jogo, não traduz shaders e não comprova NVN.
O loader pode imprimir avisos de overlays instalados no sistema; erros de validação são identificados à parte.

## Identificação dos arquivos do usuário

```bash
PYTHONPATH=tools .venv/Scripts/python -m dkrecomp.identify "game original meu" --hash --out reports/identify_nsp
```

Lê ~6,7 GB para conferir os SHA-256 (cerca de 3,5 minutos a ~35 MiB/s nesta máquina).

## Análise da ExeFS extraída pelo usuário

```bash
PYTHONIOENCODING=utf-8 PYTHONPATH=tools .venv/Scripts/python -m dkrecomp.identify user_files/exefs --out reports/identify_exefs_v0
```

```bash
PYTHONIOENCODING=utf-8 PYTHONPATH=tools .venv/Scripts/python -m dkrecomp.coverage user_files/exefs --out reports/coverage_v0
```

O segundo mede quantas instruções de cada módulo o decodificador aceita (cerca de 20 s). Repetir depois de cada
avanço do lifter.

## Gerar, compilar e dar boot no jogo

Gera o C de todos os módulos da ExeFS, as imagens já ligadas, o `layout.c` e um `build.ninja`; tudo vai para
`generated/` (ignorada pelo Git). Os endereços dos módulos vêm de `tools/dkrecomp/link.py` e ficam gravados no C.

```bash
PYTHONIOENCODING=utf-8 PYTHONPATH=tools .venv/Scripts/python -m dkrecomp.gen user_files/exefs --out generated/dkctf_v0
```

Adicionar `--gfx` para ligar os overrides NVN e o back-end Vulkan ao executável. Sem essa opção, a geração
conserva o caminho original do SDK até os serviços/SVCs (`nvdrv` ainda não implementado). O modo gráfico exige
os headers obtidos com `tools/fetch_gfx_deps.py`; no manifesto, `graphics_hle` identifica o modo escolhido.

```bash
.venv/Scripts/ninja -C generated/dkctf_v0 -j 12
```

Executar o boot (o argumento é a pasta com os `.img`; o log também vai para `dkctf_boot.log`):

```bash
cd generated/dkctf_v0 && ./dkctf.exe .
```

O segundo argumento limita o boot em segundos. O terceiro pode selecionar uma imagem RomFS; também é possível
usar `DKR_ROMFS`. O padrão procura `user_files/romfs.bin` a partir da pasta de módulos gerados.

Ou tudo de uma vez (regenera só o que mudou, compila e roda; ~45 s quando só o runtime mudou):

```bash
tools/boot.sh 180
```

O helper usa `--gfx` por padrão. `tools/boot.sh 30 --no-gfx` reproduz o caminho legado; `--gfx` explícito
também é aceito. `DKR_GFX_HIDDEN=1` cria a janela oculta durante diagnóstico do boot. No modo gráfico,
`nv::InitializeGraphics` cria o device/swapchain do host; procs NVN não implementados param por nome.

O log mostra cada SVC e comando IPC até a primeira função pendente. O vigia encerra o processo se houver uma
espera prolongada e relata o estado de cada thread. `DKR_LOG_SYNC=1` também registra as SVCs de sincronização.
O ponto de parada medido mais recente fica em `docs/PROGRESS.md`; a contagem pode variar com o escalonamento.
O Ninja usa depfiles de Clang para recompilar objetos afetados por alterações em headers.
O boot também registra a entrada em `nnMain`, o resultado passado a `SetTerminateResult` e, numa parada,
os frames guest acessíveis por frame pointer. Esses frames ajudam a localizar um abort do SDK sem desativá-lo.
Uma exceção do host (ponteiro guest inválido, bug do runtime) é relatada da mesma forma, com o código e o
endereço da falha (`boot: host exception ...`).

### Driver NVN original do SDK

O NVN do alvo está ligado no `sdk`. Para conferir layouts e decidir o que roda como código original:

```bash
PYTHONPATH=tools .venv/Scripts/python -m dkrecomp.nvnmap user_files/exefs --purity
PYTHONPATH=tools .venv/Scripts/python -m dkrecomp.nvnmap user_files/exefs --dis "^nvnTextureBuilderSet"
```

A saída descreve o código do usuário: imprimir ou guardar em `generated/`, nunca versionar. Com `--gfx`, o
gerador grava `nvn_originals.c` com os procs puros e o manifesto conta `pure_nvn_procs`.
`tests/python/test_nvnmap.py` (sem dados do jogo) e o teste CMake `nvn_texture_layout` cobrem a análise e o
cálculo de tamanho de texturas.

Comparação diferencial dos dois builders de device contra o SDK 4.4.0 do usuário (os endereços pertencem a esse
build; imagem e relatório permanecem ignorados):

```bash
PYTHONPATH=tools .venv/Scripts/python -m dkrecomp.testing.nvn_probe \
  --image generated/dkctf_v0/sdk.img --base 0x806e00000 --memory-size 0xa89000 \
  --resolver 0x8070deef8 --ready-flag 0x807869a00 --native build/zig/dkr_nvn_test.exe \
  --cases 512 --out reports/nvn_builder_probe.json
```

O probe marca uma guarda de inicialização **somente dentro do Unicorn**, resolve as funções originais e compara
as escritas em regiões aleatórias com canaries contra o bridge nativo. Não inicializa a GPU original. O modo
binário `--builder-probe` do teste C não contém código do jogo. Medido: 512 casos, zero diferenças; SetDefaults
zera exatamente 16 bytes e SetFlags altera somente os 32 bits iniciais.

## Resultados registrados

O teste nativo `shared_memory` usa dados sintéticos para verificar aliases reais host/guest, bases de 4 KiB,
duas views no mesmo bloco de reserva, permissões read-only via `VirtualQuery`, remapeamento, referências de
handles e mappings, 100 ciclos de liberação e IPC CreateAppletResource/GetSharedMemoryHandle. O mapeamento
permanece válido após fechar todos os handles, até encerrar o processo, como exige o contrato de shared memory.
O mesmo teste confere configuração/getters HID, orientação inválida, slots Handheld/Other e eventos de estilo
copiados. `kernel_sync` também espera numa cópia do evento, sinaliza o original e confere a escolha do primeiro
índice quando dois handles no array apontam para o mesmo evento.
O teste `display_service` verifica as permissões entre vi:u/s/m, interfaces filhas, nome/resolução do display,
aberturas/fechamentos, bounds do NativeWindow, layers distintos, refcounts Binder e cópias do evento 0xf.
Inclui Connect/Disconnect do produtor por X/C e A/B, canaries, duplicação de conexão/API incorreta e rejeição
de parcels malformados (offsets/tamanho, token UTF-16, campos truncados) sem mutação da conexão.

Ver `docs/PROGRESS.md`. Regra: resultado só entra na documentação se foi produzido por um comando acima.
