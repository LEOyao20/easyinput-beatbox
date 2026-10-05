import {
  applyLocalBpmDraft,
  applyLocalPattern,
  applyLocalVolumeDraft,
  clearBpmDraft,
  clearVolumeDraft,
  createInitialState,
  markConnecting,
  markDisconnected,
  reduceHostLine,
  type DeviceState,
} from "./device-store";
import { bankHex, type PatternBanks } from "./pattern";
import { clampBpm, clampSwing, clampVolume } from "./protocol";

export type { DeviceState as BeatboxState };

type Listener = (state: DeviceState) => void;

/** Optional runtime instrumentation hook (used by the diagnostic panel). */
export type LinkDebugCounters = {
  rx: number;
  rxOk: number;
  rxBad: number;
  tx: number;
  txFail: number;
  lastTx: string;
  lastBad: string;
};
export const linkDebug: LinkDebugCounters = {
  rx: 0,
  rxOk: 0,
  rxBad: 0,
  tx: 0,
  txFail: 0,
  lastTx: "",
  lastBad: "",
};

const ESPRESSIF_VID = 0x303a;

export class BeatboxLink {
  private listeners = new Set<Listener>();
  private state: DeviceState = createInitialState();
  private port: SerialPort | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private readLoopActive = false;
  private rxText = "";
  private encoder = new TextEncoder();
  private decoder = new TextDecoder();
  private reconnectTimer: number | null = null;
  private userDisconnected = false;
  private closing = false;
  private staleTimer: number | null = null;
  private commitTimer: number | null = null;
  private pendingCommitBank: 0 | 1 | 2 | null = null;

  getState() {
    return this.state;
  }

  subscribe(fn: Listener) {
    this.listeners.add(fn);
    fn(this.state);
    return () => this.listeners.delete(fn);
  }

  private setState(next: DeviceState) {
    this.state = next;
    for (const fn of this.listeners) fn(this.state);
  }

  private patch(reducer: (s: DeviceState) => DeviceState) {
    this.setState(reducer(this.state));
  }

  async connect() {
    if (!("serial" in navigator)) {
      throw new Error("当前浏览器不支持 Web Serial。请使用 Chrome / Edge，并允许串口权限。");
    }
    this.scheduleAutoReconnect();
    await this.tryExistingPorts();
  }

  async requestPort() {
    if (!("serial" in navigator)) {
      throw new Error("当前浏览器不支持 Web Serial。");
    }
    this.userDisconnected = false;
    const port = await navigator.serial.requestPort({
      filters: [{ usbVendorId: ESPRESSIF_VID }],
    });
    await this.openPort(port);
  }

  async disconnect() {
    this.userDisconnected = true;
    await this.closePort();
    this.patch((s) => markDisconnected(s, "未连接"));
  }

  private scheduleAutoReconnect() {
    if (this.reconnectTimer != null) return;
    this.reconnectTimer = window.setInterval(() => {
      if (!this.state.connected && !this.userDisconnected) {
        void this.tryExistingPorts();
      }
    }, 1500);
  }

  private async tryExistingPorts() {
    if (this.userDisconnected || this.state.connected || this.readLoopActive) return;
    const ports = await navigator.serial.getPorts();
    for (const port of ports) {
      const info = port.getInfo();
      if (info.usbVendorId != null && info.usbVendorId !== ESPRESSIF_VID) continue;
      try {
        await this.openPort(port);
        return;
      } catch {
        /* try next */
      }
    }
    if (!this.state.connected) {
      this.patch((s) => markDisconnected(s, "等待设备…点击「连接」授权串口"));
    }
  }

  private async openPort(port: SerialPort) {
    if (this.port === port && this.state.connected) return;

    await this.closePort();
    this.port = port;

    await port.open({ baudRate: 115200 });

    /*
     * Raise DTR/RTS on the ESP32-S3 USB-Serial/JTAG endpoint.
     *
     * The USB-Serial/JTAG peripheral gates HOST -> DEVICE data on the modem
     * control lines: with DTR deasserted the device sees an empty stdin and
     * `read()` never returns a byte, so every command we write is silently
     * dropped on arrival. Web Serial's `open()` does NOT assert them by
     * default, which made the device report `rx: bytes=0` forever while the
     * page happily showed "connected" (device -> host traffic was fine,
     * because that direction is not gated).
     */
    try {
      await port.setSignals({ dataTerminalReady: true, requestToSend: true });
    } catch {
      /* Non-fatal: some platforms/ports do not expose modem control lines. */
    }

    this.writer = port.writable?.getWriter() ?? null;
    this.reader = port.readable?.getReader() ?? null;
    this.readLoopActive = true;
    this.rxText = "";

    this.patch((s) => markConnecting(s));
    void this.writeLine('{"t":"ping"}');
    void this.readLoop();
    this.armStaleWatch();
  }

  private armStaleWatch() {
    if (this.staleTimer != null) window.clearInterval(this.staleTimer);
    this.staleTimer = window.setInterval(() => {
      if (!this.state.connected) return;
      if (Date.now() - this.state.updatedAt > 4000 && this.state.sync === "synced") {
        this.patch((s) => ({ ...s, sync: "stale", updatedAt: Date.now() }));
      }
    }, 1000);
  }

  private async closePort() {
    if (this.closing) return;
    this.closing = true;
    this.readLoopActive = false;
    const reader = this.reader;
    const writer = this.writer;
    const port = this.port;
    this.reader = null;
    this.writer = null;
    this.port = null;
    try {
      await reader?.cancel();
    } catch {
      /* ignore */
    }
    try {
      reader?.releaseLock();
    } catch {
      /* ignore */
    }
    try {
      writer?.releaseLock();
    } catch {
      /* ignore */
    }
    try {
      await port?.close();
    } catch {
      /* ignore */
    }
    this.closing = false;
  }

  private async readLoop() {
    const reader = this.reader;
    if (!reader) return;
    try {
      while (this.readLoopActive) {
        const { value, done } = await reader.read();
        if (done) break;
        if (!value) continue;
        this.rxText += this.decoder.decode(value, { stream: true });
        let nl: number;
        while ((nl = this.rxText.indexOf("\n")) >= 0) {
          const line = this.rxText.slice(0, nl).trim();
          this.rxText = this.rxText.slice(nl + 1);
          if (line) this.onLine(line);
        }
      }
    } catch {
      /* disconnect */
    } finally {
      this.readLoopActive = false;
      this.patch((s) =>
        markDisconnected(s, this.userDisconnected ? "未连接" : "已断开，正在重连…"),
      );
      void this.closePort();
    }
  }

  private onLine(line: string) {
    linkDebug.rx++;
    if (line.startsWith("{")) {
      try {
        JSON.parse(line);
        linkDebug.rxOk++;
      } catch {
        linkDebug.rxBad++;
        linkDebug.lastBad = line.slice(0, 120);
        console.warn("[beatbox] malformed rx line:", line.slice(0, 200));
      }
    }
    this.patch((s) => reduceHostLine(s, line));
  }

  private async writeLine(line: string) {
    if (!this.writer) {
      linkDebug.txFail++;
      linkDebug.lastTx = `(no writer) ${line}`;
      return;
    }
    linkDebug.lastTx = line;
    try {
      await this.writer.write(this.encoder.encode(line + "\n"));
      /* Counted only after a successful write: tx means "reached the port". */
      linkDebug.tx++;
    } catch {
      linkDebug.txFail++;
      this.patch((s) => markDisconnected(s, "写入失败，正在重连…"));
      void this.closePort();
    }
  }

  sendStart() {
    void this.writeLine('{"t":"start"}');
  }

  sendContinue() {
    void this.writeLine('{"t":"continue"}');
  }

  sendStop() {
    void this.writeLine('{"t":"stop"}');
  }

  sendBpm(bpm: number) {
    const v = clampBpm(bpm);
    this.patch((s) => applyLocalBpmDraft(s, v));
    void this.writeLine(`{"t":"bpm","v":${v}}`);
    window.setTimeout(() => this.patch((s) => clearBpmDraft(s)), 400);
  }

  sendSwing(swing: number) {
    const v = clampSwing(swing);
    this.patch((s) => ({ ...s, swing: v, updatedAt: Date.now() }));
    void this.writeLine(`{"t":"swing","v":${v}}`);
  }

  sendVariation(varIndex: number) {
    const v = varIndex ? 1 : 0;
    this.patch((s) => ({ ...s, variation: v, updatedAt: Date.now() }));
    void this.writeLine(`{"t":"variation","v":${v}}`);
  }

  sendFill(held: boolean) {
    this.patch((s) => ({ ...s, fill: held, updatedAt: Date.now() }));
    void this.writeLine(`{"t":"fill","v":${held ? 1 : 0}}`);
  }

  /** Arm host overdub on device: locks S7 / S8 / encoder transport. */
  sendRecord(armed: boolean) {
    void this.writeLine(`{"t":"record","v":${armed ? 1 : 0}}`);
  }

  sendNote(note: number, velocity = 127) {
    const n = note & 0x7f;
    const v = Math.max(1, velocity & 0x7f);
    this.patch((s) => ({ ...s, lastPadNote: n, updatedAt: Date.now() }));
    void this.writeLine(`{"t":"note","n":${n},"v":${v}}`);
  }

  sendClick(enabled: boolean) {
    this.patch((s) => ({ ...s, click: enabled, updatedAt: Date.now() }));
    void this.writeLine(`{"t":"click","v":${enabled ? 1 : 0}}`);
  }

  sendMode(drumMode: boolean) {
    this.patch((s) => ({ ...s, drumMode, updatedAt: Date.now() }));
    void this.writeLine(`{"t":"mode","v":${drumMode ? 1 : 0}}`);
  }

  sendVolume(volume: number) {
    const v = clampVolume(volume);
    this.patch((s) => applyLocalVolumeDraft(s, v));
    void this.writeLine(`{"t":"volume","v":${v}}`);
    window.setTimeout(() => this.patch((s) => clearVolumeDraft(s)), 400);
  }

  requestPattern() {
    void this.writeLine('{"t":"pattern_get"}');
  }

  /** Update local draft and push to device shortly after. */
  setLocalPattern(pattern: PatternBanks, bank: 0 | 1 | 2 = 0) {
    this.patch((s) => applyLocalPattern(s, pattern));
    this.pendingCommitBank = bank;
    if (this.commitTimer != null) window.clearTimeout(this.commitTimer);
    this.commitTimer = window.setTimeout(() => {
      const target = this.pendingCommitBank ?? 0;
      this.pendingCommitBank = null;
      this.commitPatternBank(target);
    }, 180);
  }

  /** Replace all banks locally and push A/B/Fill to the device. */
  setLocalPatternAll(pattern: PatternBanks) {
    this.patch((s) => applyLocalPattern(s, pattern));
    if (this.commitTimer != null) window.clearTimeout(this.commitTimer);
    this.pendingCommitBank = null;
    this.commitTimer = window.setTimeout(() => {
      this.commitPatternBank(0);
      this.commitPatternBank(1);
      this.commitPatternBank(2);
    }, 180);
  }

  commitPatternBank(bank: 0 | 1 | 2) {
    const s = this.state;
    if (!s.connected) return;
    const hex = bankHex(s.pattern, bank);
    void this.writeLine(
      `{"t":"pattern_set","bank":${bank},"rev":${s.revision},"p":"${hex}"}`,
    );
  }

  sendSave() {
    void this.writeLine('{"t":"save"}');
  }
}
