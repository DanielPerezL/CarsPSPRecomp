// Vuelca la memoria cargada y relocalizada (bloques inicializados) a un binario plano
// desde la base de la imagen, rellenando huecos con ceros.
// Uso headless: -postScript DumpMemory.java <salida.bin>
// @category Cars
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;
import java.io.RandomAccessFile;

public class DumpMemory extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        try (RandomAccessFile f = new RandomAccessFile(getScriptArgs()[0], "rw")) {
            f.setLength(0);
            for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                if (!b.isInitialized() || !b.isLoaded() || b.getStart().getOffset() < base) continue;
                byte[] data = new byte[(int) b.getSize()];
                b.getBytes(b.getStart(), data);
                f.seek(b.getStart().getOffset() - base);
                f.write(data);
            }
        }
    }
}
