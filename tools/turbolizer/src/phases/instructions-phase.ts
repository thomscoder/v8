// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import { Phase, PhaseType } from "./phase";
import { anyToString } from "../common/util";

export class InstructionsPhase extends Phase {
  // Maps node ids to instruction ranges.
  nodeIdToInstructionRange?: Array<[number, number]>;
  // Maps block ids to instruction ranges.
  blockIdToInstructionRange?: Array<[number, number]>;
  // Maps instruction offsets to PC offset.
  instructionOffsetToPCOffset?: Array<[number, number]>;
  codeOffsetsInfo?: CodeOffsetsInfo;

  // Maps instruction numbers to PC offsets.
  instructionToPCOffset: Array<TurbolizerInstructionStartInfo>;
  // Maps PC offsets to instructions.
  pcOffsetToInstructions: Map<number, Array<number>>;
  pcOffsets: Array<number>;

  constructor(name: string, nodeIdToInstructionRange?: Array<[number, number]>,
              blockIdToInstructionRange?: Array<[number, number]>,
              codeOffsetsInfo?: CodeOffsetsInfo) {
    super(name, PhaseType.Instructions);
    this.nodeIdToInstructionRange = nodeIdToInstructionRange ?? new Array<[number, number]>();
    this.blockIdToInstructionRange = blockIdToInstructionRange ?? new Array<[number, number]>();
    this.codeOffsetsInfo = codeOffsetsInfo;

    this.instructionToPCOffset = new Array<TurbolizerInstructionStartInfo>();
    this.pcOffsetToInstructions = new Map<number, Array<number>>();
    this.pcOffsets = new Array<number>();
  }

  public getKeyPcOffset(offset: number): number {
    if (this.pcOffsets.length === 0) return -1;
    for (const key of this.pcOffsets) {
      if (key <= offset) {
        return key;
      }
    }
    return -1;
  }

  public instructionToPcOffsets(instruction: number): TurbolizerInstructionStartInfo {
    return this.instructionToPCOffset[instruction];
  }

  public instructionsToKeyPcOffsets(instructionIds: Iterable<number>): Array<number> {
    const keyPcOffsets = new Array<number>();
    for (const instructionId of instructionIds) {
      const pcOffset = this.instructionToPCOffset[instructionId];
      if (pcOffset !== undefined) keyPcOffsets.push(pcOffset.gap);
    }
    return keyPcOffsets;
  }

  public nodesForPCOffset(offset: number): Array<string> {
    if (this.pcOffsets.length === 0) return new Array<string>();
    for (const key of this.pcOffsets) {
      if (key <= offset) {
        const instructions = this.pcOffsetToInstructions.get(key);
        const nodes = new Array<string>();
        for (const instruction of instructions) {
          for (const [nodeId, range] of this.nodeIdToInstructionRange.entries()) {
            if (!range) continue;
            const [start, end]  = range;
            if (start == end && instruction == start) {
              nodes.push(anyToString(nodeId));
            }
            if (start <= instruction && instruction < end) {
              nodes.push(anyToString(nodeId));
            }
          }
        }
        return nodes;
      }
    }
    return new Array<string>();
  }

  public nodesToKeyPcOffsets(nodeIds: Set<string>): Array<TurbolizerInstructionStartInfo> {
    let offsets = new Array<TurbolizerInstructionStartInfo>();
    for (const nodeId of nodeIds) {
      const range = this.nodeIdToInstructionRange[nodeId];
      if (!range) continue;
      offsets = offsets.concat(this.instructionRangeToKeyPcOffsets(range));
    }
    return offsets;
  }

  public getInstruction(nodeId: number): [number, number] {
    return this.nodeIdToInstructionRange[nodeId] ?? [-1, -1];
  }

  public getInstructionRangeForBlock(blockId: number): [number, number] {
    return this.blockIdToInstructionRange[blockId] ?? [-1, -1];
  }

  public getInstructionKindForPCOffset(offset: number): InstructionKind {
    if (this.codeOffsetsInfo) {
      if (offset >= this.codeOffsetsInfo.deoptimizationExits) {
        if (offset >= this.codeOffsetsInfo.pools) {
          return InstructionKind.Pools;
        } else if (offset >= this.codeOffsetsInfo.jumpTables) {
          return InstructionKind.JumpTables;
        } else {
          return InstructionKind.DeoptimizationExits;
        }
      }
      if (offset < this.codeOffsetsInfo.deoptCheck) {
        return InstructionKind.CodeStartRegister;
      } else if (offset < this.codeOffsetsInfo.initPoison) {
        return InstructionKind.DeoptCheck;
      } else if (offset < this.codeOffsetsInfo.blocksStart) {
        return InstructionKind.InitPoison;
      }
    }
    const keyOffset = this.getKeyPcOffset(offset);
    if (keyOffset != -1) {
      const infos = this.pcOffsetToInstructions.get(keyOffset)
        .map(instrId => this.instructionToPCOffset[instrId])
        .filter(info => info.gap !== info.condition);
      if (infos.length > 0) {
        const info = infos[0];
        if (!info || info.gap == info.condition) return InstructionKind.Unknown;
        if (offset < info.arch) return InstructionKind.Gap;
        if (offset < info.condition) return InstructionKind.Arch;
        return InstructionKind.Condition;
      }
    }
    return InstructionKind.Unknown;
  }

  public instructionKindToReadableName(instructionKind: InstructionKind): string {
    switch (instructionKind) {
      case InstructionKind.CodeStartRegister:
        return "Check code register for right value";
      case InstructionKind.DeoptCheck:
        return "Check if function was marked for deoptimization";
      case InstructionKind.InitPoison:
        return "Initialization of poison register";
      case InstructionKind.Gap:
        return "Instruction implementing a gap move";
      case InstructionKind.Arch:
        return "Instruction implementing the actual machine operation";
      case InstructionKind.Condition:
        return "Code implementing conditional after instruction";
      case InstructionKind.Pools:
        return "Data in a pool (e.g. constant pool)";
      case InstructionKind.JumpTables:
        return "Part of a jump table";
      case InstructionKind.DeoptimizationExits:
        return "Jump to deoptimization exit";
    }
    return null;
  }

  public parseNodeIdToInstructionRangeFromJSON(nodeIdToInstructionJson): void {
    if (!nodeIdToInstructionJson) return;
    for (const [nodeId, range] of Object.entries<[number, number]>(nodeIdToInstructionJson)) {
      this.nodeIdToInstructionRange[nodeId] = range;
    }
  }

  public parseBlockIdToInstructionRangeFromJSON(blockIdToInstructionRangeJson): void {
    if (!blockIdToInstructionRangeJson) return;
    for (const [blockId, range] of
      Object.entries<[number, number]>(blockIdToInstructionRangeJson)) {
      this.blockIdToInstructionRange[blockId] = range;
    }
  }

  public parseInstructionOffsetToPCOffsetFromJSON(instructionOffsetToPCOffsetJson): void {
    if (!instructionOffsetToPCOffsetJson) return;
    for (const [instruction, numberOrInfo] of Object.entries<number |
      TurbolizerInstructionStartInfo>(instructionOffsetToPCOffsetJson)) {
      let info: TurbolizerInstructionStartInfo = null;
      if (typeof numberOrInfo === "number") {
        info = new TurbolizerInstructionStartInfo(numberOrInfo, numberOrInfo, numberOrInfo);
      } else {
        info = new TurbolizerInstructionStartInfo(numberOrInfo.gap, numberOrInfo.arch,
          numberOrInfo.condition);
      }
      this.instructionToPCOffset[instruction] = info;
      if (!this.pcOffsetToInstructions.has(info.gap)) {
        this.pcOffsetToInstructions.set(info.gap, new Array<number>());
      }
      this.pcOffsetToInstructions.get(info.gap).push(Number(instruction));
    }
    this.pcOffsets = Array.from(this.pcOffsetToInstructions.keys()).sort((a, b) => b - a);
  }

  public parseCodeOffsetsInfoFromJSON(codeOffsetsInfoJson: CodeOffsetsInfo): void {
    if (!codeOffsetsInfoJson) return;
    this.codeOffsetsInfo = new CodeOffsetsInfo(codeOffsetsInfoJson.codeStartRegisterCheck,
      codeOffsetsInfoJson.deoptCheck, codeOffsetsInfoJson.initPoison,
      codeOffsetsInfoJson.blocksStart, codeOffsetsInfoJson.outOfLineCode,
      codeOffsetsInfoJson.deoptimizationExits, codeOffsetsInfoJson.pools,
      codeOffsetsInfoJson.jumpTables);
  }

  private instructionRangeToKeyPcOffsets([start, end]: [number, number]):
    Array<TurbolizerInstructionStartInfo> {
    if (start == end) return [this.instructionToPCOffset[start]];
    return this.instructionToPCOffset.slice(start, end);
  }
}

export class CodeOffsetsInfo {
  codeStartRegisterCheck: number;
  deoptCheck: number;
  initPoison: number;
  blocksStart: number;
  outOfLineCode: number;
  deoptimizationExits: number;
  pools: number;
  jumpTables: number;

  constructor(codeStartRegisterCheck: number, deoptCheck: number, initPoison: number,
              blocksStart: number, outOfLineCode: number, deoptimizationExits: number,
              pools: number, jumpTables: number) {
    this.codeStartRegisterCheck = codeStartRegisterCheck;
    this.deoptCheck = deoptCheck;
    this.initPoison = initPoison;
    this.blocksStart = blocksStart;
    this.outOfLineCode = outOfLineCode;
    this.deoptimizationExits = deoptimizationExits;
    this.pools = pools;
    this.jumpTables = jumpTables;
  }
}

export class TurbolizerInstructionStartInfo {
  gap: number;
  arch: number;
  condition: number;

  constructor(gap: number, arch: number, condition: number) {
    this.gap = gap;
    this.arch = arch;
    this.condition = condition;
  }
}

export enum InstructionKind {
  Pools = "pools",
  JumpTables = "jump-tables",
  DeoptimizationExits = "deoptimization-exits",
  CodeStartRegister = "code-start-register",
  DeoptCheck = "deopt-check",
  InitPoison = "init-poison",
  Gap = "gap",
  Arch = "arch",
  Condition = "condition",
  Unknown = "unknown"
}
