/// <reference types="svelte" />
/// <reference types="vite/client" />
declare module "*.plan?raw" {
  const content: string;
  export default content;
}
