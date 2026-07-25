// Type definitions for node-native-win-audio.

declare module 'node-native-win-audio' {
  export enum Backend {
    WASAPI           = 'wasapi',
    WASAPI_EXCLUSIVE = 'wasapi-exclusive',
    ASIO             = 'asio',
    MME              = 'mme',
    WDM              = 'wdm',
    AUDIOGRAPH       = 'audiograph',
  }

  export enum Direction {
    INPUT  = 'input',
    OUTPUT = 'output',
  }

  export enum Format {
    U8     = 'u8',
    S16    = 's16',
    S24    = 's24',
    S24_32 = 's24-32',
    S32    = 's32',
    F32    = 'f32',
    F64    = 'f64',
  }

  export interface DeviceInfo {
    id: string;
    name: string;
    adapter: string;
    direction: 'input' | 'output';
    maxInputChannels: number;
    maxOutputChannels: number;
    isDefaultInput: boolean;
    isDefaultOutput: boolean;
    supportedSampleRates: number[];
    supportedFormats: string[];
  }

  export interface StreamConfig {
    backend?: string;
    direction?: 'input' | 'output';
    deviceId?: string;
    sampleRate?: number;
    channels?: number;
    format?: string;
    bufferFrames?: number;
    numBuffers?: number;
    exclusive?: boolean;
    eventDriven?: boolean;
  }

  export interface FormatSupportResult {
    supported: boolean;
    nearest: {
      format: string;
      sampleRate: number;
      channels: number;
    };
  }

  export class Stream {
    constructor(cfg: Required<StreamConfig>);
    on(event: 'data', listener: (data: Float32Array) => void): this;
    on(event: 'error', listener: (err: Error) => void): this;
    off(event: 'data' | 'error', listener: Function): this;
    start(): this;
    stop(): this;
    close(): void;
    write(samples: Float32Array): number;
    read(): Float32Array | null;
    flush(): this;
    latencyFrames(): number;
    latencySeconds(): number;
    xruns(): number;
    streamTime(): number;
    bufferedFrames(): number;
    readonly backend: string;
    readonly direction: string;
    readonly sampleRate: number;
    readonly channels: number;
    readonly format: string;
    readonly bufferFrames: number;
  }

  export function listDevices(
    backend?: string,
    direction?: 'input' | 'output'
  ): DeviceInfo[];
  export function createStream(cfg?: StreamConfig): Stream;
  export function isFormatSupported(cfg: StreamConfig): FormatSupportResult;
  export function backends(): string[];
}
