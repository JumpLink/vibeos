// What GJS and GNOME Shell put in every module's scope, for undefined-names.sh: anything else a module uses it must
// declare or import itself.
declare const global: any;
declare const imports: any;
declare const ARGV: string[];
declare function log(...args: any[]): void;
declare function logError(...args: any[]): void;
declare function print(...args: any[]): void;
declare function printerr(...args: any[]): void;
declare const TextDecoder: any;
declare const TextEncoder: any;
