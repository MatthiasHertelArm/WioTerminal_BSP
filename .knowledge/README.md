# Knowledge of SeeedStudio::WioTerminal_BSP for the CMSIS Developer Assistant

`units/` holds the knowledge units of this pack, one Markdown file each: YAML frontmatter (`id`, `type`, `title`,
`summary`, `status`, `applies_to`, optional `symptoms`/`symptom_regex` for troubleshooting units, `source` and
`license`), followed by the text. The format is described in
[cmsis-developer-assistant-tools](https://github.com/MatthiasHertelArm/cmsis-developer-assistant-tools/blob/main/docs/format.md).
The units were written while creating and validating the pack on a Wio Terminal with a ULINKplus, CMSIS-Toolbox
2.15, Arm Compiler 6.24, GCC 14.3.1 and pyOCD 0.46.0.

`gen_pack.sh` builds them into `knowledge.db`, the index the assistant's knowledge tools read, and ships both:

```sh
pip install cmsis-developer-assistant-tools
cmsis-knowledge build --strict --fetch .
```

The assistant offers these units only in projects that use this pack.

| File | Type | Topic |
|:-----|:-----|:------|
| `units/ts-pyocd-samd51-erase-0x0-bootprot.md` | troubleshooting | pyOCD `flash erase sector failure (address 0x00000000; result code 0x1)` with the DFP flash algorithm |
| `units/ts-pyocd-samd51-hardware-reset-ack0.md` | troubleshooting | `Unexpected ACK '0'` after a hardware reset; the DFP lacks a ResetHardware sequence |
| `units/concept-wio-terminal-boot-loader.md` | concept | UF2 boot loader, BOOTPROT, application at `0x00004000`, double-tap window |
| `units/concept-wio-terminal-hardware.md` | concept | Wio Terminal parts, debug access, pin map |
| `units/proc-wio-terminal-bring-up.md` | procedure | Build, download and run to `main` on the Wio Terminal |
