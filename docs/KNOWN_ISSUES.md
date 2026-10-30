# Problemas conhecidos

## Bloqueios

| # | Problema | Impacto | Saída |
|---|---|---|---|
| B1 | O código do jogo está em NCAs criptografados e o DKRecomp não decripta | Sem análise do `main`; MVP 2b+ bloqueados | O usuário fornece a ExeFS/RomFS extraídas por conta própria (`docs/REQUIRED_FILES.md`) |
| B2 | O Visual Studio 2026 desta máquina não tem o Windows SDK (falta `kernel32.lib` e os headers da UCRT) | MSVC não compila nem liga; a compatibilidade MSVC do C gerado não foi verificada | Instalar o componente "Windows 11 SDK" pelo Visual Studio Installer (mudança no sistema: **requer autorização do usuário**). Enquanto isso, Clang do Zig |

## Limitações do recompilador

- **Aritmética SIMD/FP não é suportada** (FADD, FMUL, FCVT, SCVTF, FCMP, vetores inteiros, BSL...). Só
  loads/stores SIMD, FMOV entre registradores gerais e SIMD, INS, UMOV, DUP, MOVI 64-bit e AND/BIC/ORR/ORN/EOR
  vetoriais. O jogo quase certamente usa muito SIMD/FP (hipótese H6).
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
