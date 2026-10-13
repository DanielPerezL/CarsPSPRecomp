// Renombra los stubs de import con el CSV de tools/psp_imports.py (dirección,librería,NID,nombre).
// Uso headless: -postScript ApplyImports.java <ruta/imports.csv>
// @category Cars
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import java.nio.file.*;

public class ApplyImports extends GhidraScript {
    @Override
    public void run() throws Exception {
        Path csv = Paths.get(getScriptArgs().length > 0 ? getScriptArgs()[0] : askFile("imports.csv", "Abrir").getPath());
        int renamed = 0;
        for (String line : Files.readAllLines(csv)) {
            String[] f = line.split(",");
            Address addr = toAddr(Long.parseLong(f[0], 16));
            Function fn = getFunctionAt(addr);
            if (fn == null) fn = createFunction(addr, f[3]);
            if (fn == null) continue;
            fn.setName(f[3], SourceType.IMPORTED);
            fn.setComment(f[1] + " NID 0x" + f[2]);
            renamed++;
        }
        println("Imports renombrados: " + renamed);
    }
}
