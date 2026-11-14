# CANAIS MIDI (SMKSYNTH 1.0.3-ch)

Os 16 pads viram seletores de canal MIDI: pad 1 = canal 1 … pad 16 = canal 16 (fileira de cima 1–8, de baixo 9–16).
Pad aceso = canal ligado e enviando; apagado = canal sem enviar. Vários ao mesmo tempo, até **8**.

## Uso

- **Entrar:** toque em **KNOB BANK** até a tela mostrar **PADS CANAIS MIDI**. A ordem é PERFORMANCE → SPLIT/LAYER →
  PATTERNS → (DRUM PADS, se a parte for DRUM-X) → **CANAIS MIDI** → PERFORMANCE. Segurar KNOB BANK volta direto a
  PERFORMANCE, como antes.
- **Ligar/desligar:** um toque no pad. A faixa inferior da tela mostra os canais ligados (ex.: `1 8 15`).
- **Limite de 8:** o nono pad pisca em vermelho, a tela diz `MAXIMO 8 CANAIS` e nada muda. Desligue um para ligar outro.
- **Nenhum pad aceso:** o teclado volta ao comportamento original, saindo pelo canal da parte selecionada. O pad
  desse canal fica levemente aceso como referência.
- Os canais escolhidos continuam valendo quando você sai da página (os pads voltam às outras funções). **Não são
  salvos** ao desligar o teclado: ele liga sempre no comportamento original.

## O que vai para os canais ligados

- Teclas (as 37, com velocity), pads de bateria (DRUM PADS) e pads tocados ao vivo com REC: note-on e note-off.
- **Só controlador:** com qualquer canal aceso, o SMK não toca o som interno (Local Off): as teclas e os pads vão
  só para os módulos. Arpejador, CHORD e gravação REC do SMK também ficam fora, porque dependem do som interno.
  Com todos apagados, o SMK volta a ser o sintetizador normal. Uma nota iniciada com o som interno termina nele
  mesmo que você acenda um canal antes de soltar a tecla.
- Saída pelo **USB** e pela tomada **TRS MIDI OUT** (3,5 mm, tipo A): a mesma mensagem nas duas, com ou sem USB ligado.
  Pela TRS o SMK vai direto a uma interface, patchbay ou módulo, sem computador (adaptador TRS tipo A → DIN 5 pinos).
  A TRS transmite a 31250 baud: um acorde de 4 notas em 8 canais leva ~31 ms do primeiro ao último canal.
- Pitch bend, mod wheel, pedal e faders não saem por MIDI no SMKSYNTH, nem antes desta função.

## Garantias contra notas presas

- Cada nota guarda os canais em que começou; o note-off vai exatamente para eles, mesmo que você mude os pads no meio.
- Desligar um canal com notas seguradas encerra essas notas nesse canal na hora.

## Instalação

Este pacote é uma **atualização** sobre o SMKSYNTH 1.0.3 já instalado (preserva padrões, favoritos e cenas).

1. Se o teclado ainda está no firmware da M-VAVE: instale o SMKSYNTH 1.0.3 oficial pelo instalador do site
   (ele exige o V016 e faz a primeira instalação). Faça o backup que ele oferece.
2. Faça também o backup completo da flash (calibração das teclas e pads), como descrito em SMK37.md.
3. Com o SMKSYNTH 1.0.3 rodando: `python3 tools/fm1_install.py obervave-1.0.3-ch-smk37.fwsc`
   (Python 3 com `pip install mido python-rtmidi`). Identidade do pacote: `SMK-37 Pro_21001`.
4. Para voltar: instale de novo o SMKSYNTH 1.0.3 oficial pelo site.

## Testes

`sh tests/run_canais_midi_tests.sh`: 45 verificações no código real (canais, limite, note-offs, notas seguradas,
retorno ao canal da parte, pads de bateria, ordem do KNOB BANK, largura dos textos na tela) e um teste de ponta a
ponta pelo laço principal do firmware. `tests/run_physical_controls_tests.sh` e `tests/run_sl1_tests.sh` continuam
passando. Ainda **não testado no teclado real**. O driver da TRS (do Skiff 2.5.1) segue o firmware de fábrica
V016, mas também não foi confirmado num SMK real por nenhum projeto: é o primeiro teste a fazer.
