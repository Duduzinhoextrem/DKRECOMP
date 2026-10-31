# Problemas conhecidos

## Bloqueios

| # | Problema | Impacto | Saída |
|---|---|---|---|
| B1 | O código do jogo está em NCAs criptografados e o DKRecomp não decripta | Sem análise do `main`; MVP 2b+ bloqueados | O usuário fornece a ExeFS/RomFS extraídas por conta própria (`docs/REQUIRED_FILES.md`) |
| B2 | O Visual Studio 2026 desta máquina não tem o Windows SDK (falta `kernel32.lib` e os headers da UCRT) | MSVC não compila nem liga; a compatibilidade MSVC do C gerado não foi verificada | Instalar o componente "Windows 11 SDK" pelo Visual Studio Installer (mudança no sistema: **requer autorização do usuário**). Enquanto isso, Clang do Zig |

## Limitações do recompilador

- O decodificador cobre 100% do `main` e do `subsdk0`. **No `sdk` faltam** as extensões de criptografia (AES,
  SHA-1, SHA-256, PMULL), CRC32, `CNTPCT_EL0`, `CTR_EL0` e formas saturadas (`sqadd`, `sqrshrn`): 67 funções.
- Os auxiliares SIMD (`simd.h`) usam laços genéricos por elemento: corretos, mas lentos. Especializar por tamanho de
  elemento é trabalho de desempenho para depois do primeiro boot.
- **Só FPCR = 0 é modelado**: arredondamento ao mais próximo, sem flush-to-zero e sem modo default-NaN. Se o jogo
  escrever outro valor em FPCR, o resultado de FP pode divergir. Ainda não há aviso em tempo de execução para isso.
  As flags acumuladas de exceção do FPSR não são mantidas.
- O C gerado exige **CPU com FMA** (`-mfma`, Haswell/Zen ou mais novo), porque o FMADD precisa de multiplicação-soma
  fundida exata. É o mesmo requisito do TotK Recomp e do LAS Recomp.
- Não suportados também: `DC ZVA` (o `DCZID_EL0` informa proibido), CRC32, LSE (ARMv8.1, não esperado no Switch),
  pares exclusivos (`LDXP/STXP`), `CAS`, instruções de sistema além de NZCV/TPIDR/TPIDRRO/FPCR/FPSR/CNTVCT/CNTFRQ.
- Sobreposições de registradores CONSTRAINED UNPREDICTABLE em loads/stores (ex.: `ldp x0, x0`) são rejeitadas de
  propósito.
- `RET` é tratado como retorno da função C, e `BL` assume que o chamado retorna ao LR. Código que manipula o LR para
  retornar a outro lugar (raro, ex.: `longjmp`) não é suportado ainda.
- `BR` com alvo desconhecido dentro da função usa um `switch` sobre todos os endereços da função: correto, mas gera
  muito código. Falta resolver tabelas de salto estaticamente.
- Funções são conhecidas só por símbolos (ELF de teste). Descoberta de funções em NSO sem símbolos ainda não existe.
- O monitor exclusivo guarda um endereço por CPU e usa compare-and-swap no `STXR`; isso tem o problema ABA em
  teoria (o mesmo do TotK/LAS). Não testado com várias threads.
- O contador `CNTVCT_EL0` usa 19,2 MHz (frequência do contador do Switch documentada no switchbrew), escalado do
  `QueryPerformanceCounter`; não foi comparado com hardware.
- Floating point de host: nada de FPCR/arredondamento é aplicado ainda (não há aritmética FP).

## Boot (runtime/src/horizon.c)

- Uma única thread; SVCs de sincronização respondem como se não houvesse disputa. Basta para o boot atual, não para
  o jogo.
- O espaço de endereços (regiões de heap, alias, pilha, ASLR) e o tamanho de memória informados ao jogo foram
  escolhidos pelo DKRecomp; a memória total (`0xCD500000`) é uma aproximação.
- A entropia de `GetInfo(11)` é fixa, para que as execuções sejam reproduzíveis.
- A ordem de busca de símbolos na ligação (main, subsdk0, sdk; primeira definição global vence) segue o ELF padrão;
  que o `rtld` da Nintendo faça igual é hipótese.
- Funções descobertas por ponteiros podem começar em dados (algumas palavras inválidas no `subsdk0`/`main`); só
  causam erro se executadas.

## Módulo gerado

- O endereço base do `main` (`0x800000000`) é **provisório** e fica embutido no C gerado; mudar o layout exige
  regerar e recompilar (~5 minutos).
- 5.725 funções usam o despacho local por `switch` em `BR` (tabelas de salto não resolvidas estaticamente) e
  37.249 chamadas passam por `rt_dispatch`: correto, mas pesa no tamanho e no desempenho.
- As 830 funções descobertas por alvo de `BL` vão até o próximo início conhecido; uma delas pode ter fim impreciso.
- Nada do código gerado foi executado ainda; a correção dele só é garantida instrução a instrução pelos testes
  diferenciais.

## Testes

- O teste de função usa `-fno-vectorize` e evita `__builtin_popcountll`, porque o Clang gera NEON para eles.
- O Unicorn mantém o monitor exclusivo entre execuções; o runner executa `CLREX` antes de cada caso para começar do
  mesmo estado que o harness. (Sem isso, 1 caso em ~60 mil falhava por vazamento de estado do Unicorn.)
- No Windows o Unicorn provoca uma exceção de acesso tratada internamente durante `mem_map`; o `faulthandler` do
  pytest foi desligado em `pytest.ini` para não reportá-la como fatal.
- O Zig ignora `-mgeneral-regs-only` e liga o UBSan em `-O0`; os scripts passam as opções necessárias.

## Ferramentas

- `RequiredSystemVersion` do meta da atualização vale 3694008467456 no XML, que não é uma palavra de versão de
  32 bits. Significado não confirmado; o relatório mostra o valor bruto.
- A versão exibida ("1.0.2"?) da atualização só pode ser confirmada com o `control.nacp`.
