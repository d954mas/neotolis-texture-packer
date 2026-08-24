import Phaser from "phaser";

class RuntimeFormatsScene extends Phaser.Scene {
  preload() {
    this.load.multiatlas("runtime-formats", "golden/phaser/atlas.json");
  }

  create() {
    this.add.text(16, 16, "Phaser 3 Multi-Atlas", {
      color: "#ffffff",
      fontFamily: "sans-serif",
      fontSize: "20px",
    });
    this.add.image(64, 72, "runtime-formats", "coin").setScale(8);
    this.add.image(160, 72, "runtime-formats", "hero").setScale(8);
  }
}

new Phaser.Game({
  type: Phaser.AUTO,
  parent: "game",
  width: 256,
  height: 144,
  backgroundColor: "#151922",
  pixelArt: true,
  scene: RuntimeFormatsScene,
});
