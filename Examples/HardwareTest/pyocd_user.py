# pyOCD user script: program the internal Flash with the board's flash algorithm.
#
# pyOCD loads this file from its project directory, which is the folder of the solution for the CMSIS Load,
# Run and Erase tasks of VS Code. It is not needed with Microchip::SAMD51_DFP versions whose SAMD51_512.FLM
# describes 8 KB sectors.
#
# The SAMD51_512.FLM of Microchip::SAMD51_DFP@3.9.276 declares the whole 512 KB Flash as one sector, while its
# EraseSector erases only the 8 KB block at the given address. pyOCD then always erases the block at 0x00000000,
# which the BOOTPROT fuse protects for the UF2 boot loader, and fails. SeeedStudio::WioTerminal_BSP describes
# the same algorithm with 8 KB sectors as a board algorithm. csolution lists it in *.cbuild-run.yml after the
# device algorithm, and pyOCD uses the first algorithm that covers a region, so this script selects the board
# algorithm for the regions it covers.

import os
import yaml


def _board_algorithms():
    cbuild_run = session.options.get('cbuild_run')
    if not cbuild_run:
        return []
    base = os.path.dirname(os.path.abspath(cbuild_run))
    with open(cbuild_run, encoding='utf-8') as f:
        data = yaml.safe_load(f).get('cbuild-run', {})

    def resolve(path):
        # pyOCD has set CMSIS_PACK_ROOT in the environment when it read the cbuild-run file
        path = os.path.expanduser(os.path.expandvars(path))
        return os.path.realpath(path if os.path.isabs(path) else os.path.join(base, path))

    board_pack = data.get('board-pack-path')
    if not board_pack:
        return []
    board_pack = resolve(board_pack)
    algorithms = []
    for entry in data.get('programming', []):
        algorithm = resolve(entry.get('algorithm', ''))
        if algorithm.startswith(board_pack + os.sep):
            algorithms.append((entry['start'], entry['start'] + entry['size'], algorithm))
    return algorithms


def will_connect():
    for start, end, algorithm in _board_algorithms():
        for region in target.memory_map.iter_matching_regions(type=MemoryType.FLASH):
            if region.start >= start and region.end < end:
                region.flm = algorithm
