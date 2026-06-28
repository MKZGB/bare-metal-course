# Curso de Sistemas Embebidos Bare-Metal — STM32VLDISCOVERY (QEMU)

Um curso autónomo e prático de programação embebida **bare-metal** na placa
**STM32VLDISCOVERY** (chip **STM32F100RBT6**, ARM Cortex-M3), feito para correr
inteiramente dentro do **QEMU** — sem necessidade de hardware físico. Escreves cada
driver de raiz, diretamente sobre os registos do hardware (sem HAL, sem CMSIS, sem
biblioteca do fabricante), tal como o firmware real é construído quando precisas de
compreender e controlar cada byte.

> **Primeira vez aqui? Abre [`01-arm-architecture/01-arm-architecture-pt.html`](01-arm-architecture/01-arm-architecture-pt.html) num browser e começa a ler.** Cada módulo é uma página HTML; os módulos ligam-se entre si e ao [glossário](glossary-pt.html).
>
> 🌐 **Duas línguas.** Cada módulo existe em inglês (`NN-nome.html`) e português (`NN-nome-pt.html`), com um seletor de idioma na barra superior de cada página. A versão inglesa começa em [`README.md`](README.md). As soluções de referência em C na pasta `solutions/` são partilhadas pelas duas línguas (o código é neutro quanto ao idioma).

---

## Secção 1 — O que é este curso

### Objetivo
Pegar num programador que **sabe C e arquitetura de computadores genérica** mas que
**não tem experiência de ARM nem de bare-metal** e torná-lo capaz de escrever drivers
ao nível dos registos para um microcontrolador ARM Cortex-M: GPIO, UART,
temporizadores, interrupções, DMA, I²C e SPI — compreendendo não só o *como* mas
também o *porquê*.

### Como ensina
O curso **não escreve o código por ti.** Cada módulo dá-te:
1. **Teoria** — o conceito, os registos relevantes e os seus campos de bits (com os
   endereços exatos do manual de referência do STM32F100, o **RM0041**) e diagramas.
2. **Um exercício guiado** — um objetivo, os registos a usar, um **esqueleto `.c` com
   marcadores `TODO` que tu preenches**, **pistas passo a passo escondidas até
   clicares** e o comando exato para compilar e correr.
3. **Perguntas de verificação** — com respostas escondidas até clicares.

As soluções de referência completas estão em [`solutions/`](solutions/). Estão lá se
ficares mesmo encravado — mas aprendes muito mais se lutares primeiro com o exercício.

### Placa e chip alvo
| Propriedade | Valor |
|----------|-------|
| Placa | STM32VLDISCOVERY |
| MCU | STM32F100RBT6 ("Value Line") |
| Núcleo | ARM Cortex-M3 (ARMv7-M, Thumb-2), little-endian |
| Flash | 128 KB @ `0x08000000` |
| SRAM | 8 KB @ `0x20000000` |
| Relógio por omissão | HSI interno de 8 MHz (sem PLL configurada neste curso) |
| Manual de referência | **RM0041** (STM32F100xx) |
| LEDs | LD3 verde = **PC9**, LD4 azul = **PC8** |
| Botão de utilizador | B1 = **PA0** (ativo a nível alto) |

### O que o QEMU consegue e não consegue emular (lê isto — é essencial)
A máquina `stm32vldiscovery` do QEMU emula o **núcleo do CPU e as USARTs fielmente**,
mas **não é um modelo completo do chip**. Verifiquei cada periférico empiricamente; esta
tabela diz-te o que vais realmente observar, para nunca perderes tempo a achar que o
teu código correto está "avariado":

| Periférico | Emulado no QEMU? | O que vês |
|-----------|:-----------------:|--------------|
| **USART1/2/3** | ✅ Completo | E/S série real. **É o teu principal canal de saída.** |
| **SysTick** | ✅ Completo | Conta e interrompe corretamente. O Módulo 08 é totalmente observável. |
| **NVIC / exceções** | ✅ Completo | As interrupções são despachadas corretamente (ex.: a IRQ de receção do UART funciona). |
| **GPIO** (todos os portos) | ❌ Nenhum | As escritas são descartadas; `IDR`/`ODR`/`CRL`/`CRH` leem `0`. **Nenhum LED acende, o botão lê sempre 0.** O teu código continua correto para hardware real. |
| **TIM2/3/4** | ❌ Nenhum | O contador fica congelado em 0; a flag de atualização (`UIF`) nunca ativa. |
| **DMA1** | ❌ Nenhum | Não ocorre transferência; a flag de conclusão nunca ativa. |
| **I²C1/2** | ❌ Nenhum | Os registos leem 0; não existe dispositivo externo (ex.: um ADXL345) com quem falar. |
| **SPI1** | ⚠️ Parcial | Os registos leem-se de volta e as flags `TXE`/`RXNE` comportam-se, mas **não há escravo**, por isso os dados recebidos não têm significado. |

**Consequência:** os módulos de periféricos não emulados (GPIO, temporizadores, DMA,
I²C) ensinam a **máquina de estados ao nível dos registos** e estão escritos de modo a
que o programa **nunca bloqueie** à espera de uma flag que o QEMU nunca vai ativar —
explica a limitação e segue em frente. O código do driver está correto para uma
STM32VLDISCOVERY real; o QEMU simplesmente não modela o silício. Os módulos 06–08 e 10
dão-te resultados genuinamente visíveis e interativos.

> **A armadilha mais importante do QEMU** (o Módulo 06 trata-a em detalhe): a máquina
> tem **três** USARTs, por isso um simples `-serial stdio` falha com
> *"cannot use stdio by multiple character devices"*. Tens de dar um backend às três.
> O alvo `run` do Makefile partilhado já faz isto por ti:
> ```
> qemu-system-arm -M stm32vldiscovery -display none -serial stdio -serial null -serial null -kernel build/firmware.elf
> ```
> Se alguma vez a saída UART não aparecer, este encaminhamento (USART1 → stdio) é a
> solução. Sai do QEMU com <kbd>Ctrl-A</kbd> e depois <kbd>X</kbd>.

---

## Secção 2 — Como usar

### Pré-requisitos
- À-vontade com C (ponteiros, operadores bit a bit, `volatile`, tipos de largura fixa).
- Literacia básica de arquitetura de computadores (registos, memória, pilha).
- Linux Ubuntu/Debian (os comandos abaixo assumem-no; qualquer Linux serve).

### Instalação da toolchain
Precisas do compilador cruzado ARM e do QEMU:

```bash
# Toolchain GCC bare-metal para ARM (fornece arm-none-eabi-gcc, objdump, etc.)
sudo apt update
sudo apt install gcc-arm-none-eabi

# QEMU com emulação de sistema ARM (fornece qemu-system-arm)
sudo apt install qemu-system-arm

# Verificar
arm-none-eabi-gcc --version
qemu-system-arm -M help | grep stm32vldiscovery
```

Se não puderes usar o `apt` (ex.: sem root), descarrega a **Arm GNU Toolchain**
pré-compilada (`arm-none-eabi`) de <https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads>,
extrai-a para qualquer pasta e passa o caminho à compilação com
`PREFIX=/caminho/para/bin/arm-none-eabi-` (ver abaixo).

### Estrutura do projeto
```
bare-metal-course/
├── README.md                 ← versão inglesa
├── README-pt.md              ← estás aqui
├── glossary.html             ← glossário (EN)
├── glossary-pt.html          ← glossário (PT)
├── common/                   ← infraestrutura partilhada (reutilizada por todos os módulos)
│   ├── startup.c             ← tabela de vetores + reset handler (init .data/.bss)
│   ├── linker.ld             ← mapa de memória (Flash/SRAM) + layout das secções
│   ├── style.css             ← estilo partilhado das páginas
│   └── Makefile              ← uma única compilação para todos os módulos
├── solutions/                ← soluções de referência completas (espreita só se encravares)
│   ├── 04-gpio-output.c … 13-spi.c
├── 01-arm-architecture/ … 13-spi/   ← por módulo: NN-nome.html (EN) + NN-nome-pt.html (PT)
```

A pasta **`common/`** é o coração da estrutura bare-metal real: um único `startup.c`
e `linker.ld` são reutilizados por *todos* os exercícios. Nunca copias código de
compilação — apontas o Makefile partilhado ao teu ficheiro fonte.

### Como compilar e correr um exercício
Escreve o teu código num ficheiro dentro da pasta do módulo (ex.: `04-gpio-output/main.c`)
e depois, **dentro dessa pasta do módulo**:

```bash
# só compilar
make -f ../common/Makefile SRC=main.c

# compilar e correr no QEMU
make -f ../common/Makefile SRC=main.c run

# remover artefactos de compilação
make -f ../common/Makefile SRC=main.c clean
```

Para compilar/correr uma **solução de referência**, aponta o `SRC` para ela:

```bash
cd 06-uart-transmit
make -f ../common/Makefile SRC=../solutions/06-uart-transmit.c run
```

Se a tua toolchain não estiver no `PATH`, acrescenta `PREFIX`:

```bash
make -f ../common/Makefile PREFIX=/opt/arm/bin/arm-none-eabi- SRC=main.c run
```

Sai do QEMU com <kbd>Ctrl-A</kbd> e depois <kbd>X</kbd>.

### Ordem de leitura recomendada e dependências
Trabalha os módulos por ordem numérica. As dependências fortes são:

```
 01 Arquitetura ARM ─► 02 Modelo do Programador ─► 03 Registos Mapeados em Memória
                                                          │
                                                          ▼
                                                 04 Saída GPIO
                                                  │        │
                                          ┌───────┘        ▼
                                          ▼            05 Entrada GPIO
                                   06 Transmissão UART ◄────┘ (precisa do 04)
                                          │
                                          ▼
                                   07 Receção UART
                                          │
                                          ▼
                                   08 Temporizador SysTick
                                          │
                  ┌───────────────┬───────┼───────────────┐
                  ▼               ▼       ▼                ▼
        09 Temporizadores  10 Interrupções (NVIC)   (saídas de 06/08 reutilizadas
        (precisa do 08)    (precisa de 04, 06, 08)   para resultados visíveis)
                  │               │
                  └──────┬────────┘
                         ▼
                   11 DMA (TX UART) ─►  12 I²C  ─►  13 SPI
                   (precisa do 06)      (nível de registo)  (nível de registo)
```

| Módulo | Depende de | Porquê |
|--------|-----------|--------|
| 01 Arquitetura ARM | — | Fundação: RISC, load-store, Thumb-2. |
| 02 Modelo do Programador | 01 | Registos, modos, tabela de vetores, sequência de reset. |
| 03 Registos Mapeados em Memória | 02 | `volatile`, structs de registos, manipulação de bits — usados em tudo a seguir. |
| 04 Saída GPIO | 03 | Primeiro driver: gestão de relógio, `CRL/CRH`, `BSRR`. |
| 05 Entrada GPIO | 04 | Acrescenta `IDR` e a configuração de entrada. |
| 06 Transmissão UART | 04 | Primeira saída **visível**; redireciona o `printf`. Reutiliza a config AF do GPIO. |
| 07 Receção UART | 06 | Acrescenta `RXNE`/receção ao driver UART. |
| 08 Temporizador SysTick | 06 | Temporização real; imprime via UART. |
| 09 Temporizadores de Uso Geral | 08 | Base de tempo, output-compare, input-capture (nível de registo no QEMU). |
| 10 Interrupções (NVIC) | 04, 06, 08 | Tabela de vetores, ativar a NVIC, IRQs de receção UART e SysTick. |
| 11 DMA | 06 | TX UART via DMA (nível de registo no QEMU). |
| 12 I²C | 06 | Máquina de estados ao nível de registo (sem escravo no QEMU). |
| 13 SPI | 06 | Máquina de estados ao nível de registo (registos emulados, sem escravo). |

> ⚠️ **Nota sobre o "MODER":** se viste tutoriais de STM32F4, podes esperar um registo
> `MODER` para o GPIO. **O STM32F100 (uma peça da família STM32F1) não tem `MODER`.**
> Configura os pinos com `CRL`/`CRH` (4 bits por pino). Este curso usa os registos
> corretos da família F1 — ver Módulo 04.

### Onde estão as soluções de referência
[`solutions/NN-nome.c`](solutions/) — um ficheiro completo e bem comentado por módulo
de programação (04–13). Compilam e correm com o Makefile partilhado tal como o teu
próprio código. Os módulos 01–03 são conceptuais e revelam as respostas através das
pistas na própria página, em vez de um ficheiro separado.

---

*Construído e verificado com `arm-none-eabi-gcc` 13.2 e `qemu-system-arm` 8.2.
Todos os endereços de registos seguem o **RM0041** do STM32F100.*
