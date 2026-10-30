# Avisos de terceiros

## Código copiado ou adaptado

Nenhum. Todo o código do DKRecomp até agora foi escrito para o projeto. Os projetos de referência em `refs/`
(TotK Recomp, LAS Recomp, SwitchRecomp, N64Recomp) foram lidos para entender a arquitetura; nenhum trecho foi
copiado. Se algum for, ele entra nesta tabela com origem, commit, licença e arquivos de destino, e mantém seu
cabeçalho original.

| Componente | Origem e commit | Licença | Usado em |
|---|---|---|---|
| — | — | — | — |

## Ferramentas usadas no desenvolvimento e nos testes (não distribuídas)

| Ferramenta | Versão | Licença | Uso |
|---|---|---|---|
| Unicorn (pacote `unicorn` do PyPI) | 2.1.4 | GPL-2.0 (núcleo) | CPU ARM64 de referência nos testes diferenciais; nunca ligado ao que o DKRecomp gera |
| Capstone (pacote `capstone`) | 5.0.9 | BSD-3-Clause | Conferência do decodificador nos testes |
| Zig (pacote `ziglang`), com Clang/LLD e MinGW-w64 | 0.16.0 | MIT (Zig); Apache-2.0 com exceção LLVM (Clang/LLD); licenças do MinGW-w64 (domínio público/ZPL/MIT) | Compilar o C gerado, o runtime e os programas de teste |
| CMake, Ninja (pacotes do PyPI) | 4.4.4, 1.13.2 | BSD-3-Clause, Apache-2.0 | Build |
| lz4 (pacote `lz4`) | 4.4.5 | BSD-3-Clause | Descompressão rápida de NSO (opcional; há implementação própria em Python) |
| pytest | 9.2.0 | MIT | Testes |

## Referências de documentação

- Arm Architecture Reference Manual for A-profile (DDI 0487): codificação e semântica das instruções.
- switchbrew.org: formatos PFS0, NCA, ticket, NSO, NPDM, NACP; frequência do contador do sistema.
