# Avisos de terceiros

## Código copiado ou adaptado

Nenhum. Todo o código do DKRecomp até agora foi escrito para o projeto. Os projetos de referência em `refs/`
(TotK Recomp, LAS Recomp, SwitchRecomp, N64Recomp) foram lidos para entender a arquitetura; nenhum trecho foi
copiado. Se algum for, ele entra nesta tabela com origem, commit, licença e arquivos de destino, e mantém seu
cabeçalho original.

| Componente | Origem e commit | Licença | Usado em |
|---|---|---|---|
| — | — | — | — |

## Dependências gráficas locais

Obtidas por `tools/fetch_gfx_deps.py` em `toolchain/` (ignorada pelo Git). Não há instalação no sistema.
O código do back-end Vulkan foi escrito para o DKRecomp; ainda não foi copiado código gráfico dos projetos de referência.

| Componente | Origem e revisão | Licença | Usado em |
|---|---|---|---|
| Vulkan-Headers | [KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers), `v1.4.321`, commit `2cd90f9d20df57eac214c148f3aed885372ddcfe`; SHA-256 do arquivo baixado `cace1d5832fc00287c9b10b27577c41033b2c86434e884eef5c8dcea098d5118` | Apache-2.0 OR MIT; textos em `toolchain/vulkan-headers/LICENSES/`, avisos preservados nos headers | Declarações da API em `runtime/gfx/vk.c`, opção CMake `DKR_BUILD_GFX`; loader vem do driver do Windows |
| Vulkan-ValidationLayers (ferramenta de teste) | [KhronosGroup/Vulkan-ValidationLayers](https://github.com/KhronosGroup/Vulkan-ValidationLayers), snapshot oficial examinado em 2026-10-31: commit `16a40098c4279a6caf24204bf388c744bc6c30cf`, asset `694805805`, SHA-256 `09d2387d9929557a6d064bfb111681bc3b1ea6fc365482992ac42b43bfb36657` | Apache-2.0, com exceções mais permissivas descritas no LICENSE do projeto; texto salvo junto à ferramenta | Apenas validação de desenvolvimento, inclusive sincronização; não ligada ao runtime nem incluída no executável gerado |

`--validation` obtém o snapshot nightly oficial disponível no momento, verifica o digest publicado pelo GitHub
e registra commit/digest em `toolchain/vulkan-validation/source.txt`. O snapshot é variável; o registro local
identifica o que foi realmente testado. Os headers têm versão/digest fixos no script.

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
