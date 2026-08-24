import Phaser from "phaser";

class RuntimeFormatsScene extends Phaser.Scene {
  preload() {
    this.load.multiatlas("runtime-formats", "/atlas.json");
  }

  create() {
    for (const frame of ["hero", "coin"]) {
      if (!this.textures.get("runtime-formats").has(frame)) {
        throw new Error(`missing exported frame: ${frame}`);
      }
    }
    this.add.image(72, 64, "runtime-formats", "hero").setScale(8);
    this.add.image(184, 80, "runtime-formats", "coin").setScale(8);

    let renderedFrames = 0;
    this.game.events.on(Phaser.Core.Events.POST_RENDER, () => {
      if (++renderedFrames === 2) {
        window.__NTPACKER_TEST__ = { engine: "phaser", ready: true };
      }
    });
  }
}

new Phaser.Game({
  type: Phaser.WEBGL,
  parent: "game",
  width: 256,
  height: 144,
  backgroundColor: "#000000",
  antialias: false,
  pixelArt: true,
  roundPixels: true,
  resolution: 1,
  scene: RuntimeFormatsScene,
});
