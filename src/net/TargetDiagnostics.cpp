#include "net/TargetDiagnostics.h"

#include "net/InstallLog.h"

namespace camsyringe {

namespace {

// Runs on the target's ksh. Written for it (docs/adr/0010): echo, not printf (no printf binary on
// the image); no pipes into variables that must survive; every command's own error text goes to
// the log, nothing is hidden. @DIR@ and @BUNDLE@ are filled in by script().
const char* const kScript = R"SH(D=@DIR@
LOG=$D/tmp/@LOGNAME@
SUM=$D/tmp/.qcarcam_diag_summary
mkdir -p $D/tmp
: > $SUM
LD="$D/lib:/ifs/lib64/:/ifs/lib/:/mnt/lib64/:/mnt/usr/lib64/:/mnt/lib/:/mnt/usr/lib/:/mnt/lib64/camera:/ifs/lib64/camera:/mnt/dis/lib64"
{
echo "=== CamSyringe injector install diagnostics ==="
echo "written: $(date)"
echo "install dir: $D   bundle: @BUNDLE@"
echo
echo "--- target ---"
cat /etc/os-release 2>&1
uname -a 2>&1
echo
echo "--- the bundle that was run (header: the build it was made from) ---"
ls -l $D/tmp/@BUNDLE@ 2>&1
head -n 4 $D/tmp/@BUNDLE@ 2>&1
echo "installed BUILD_ID: $(cat $D/BUILD_ID 2>&1)"
echo
echo "--- disk ---"
df -k $D 2>&1
echo
echo "--- library search path (the same one the bundle's load check uses) ---"
OLDIFS=$IFS
IFS=:
for d in $LD; do
  if [ -d "$d" ]; then echo "  present  $d ($(ls $d | wc -l) entries)"; else echo "  MISSING  $d (no such directory on this target)"; fi
done
IFS=$OLDIFS
echo
echo "--- installed files against the bundle's manifest ($D/MANIFEST.md5) ---"
if [ -f $D/MANIFEST.md5 ]; then
  HAVE_MD5=0
  type md5sum >/dev/null 2>&1 && HAVE_MD5=1
  [ $HAVE_MD5 = 1 ] || echo "  (no md5sum on this target: checking that the files exist only)"
  while read sum f rest; do
    case "$sum" in "#"*|"") continue;; esac
    f=${f#\*}
    if [ ! -e "$D/$f" ]; then
      echo "  MISSING   $f"
      echo "SUMMARY: installed file $D/$f is missing (the bundle's extraction did not produce it)" >> $SUM
    elif [ $HAVE_MD5 = 1 ]; then
      got=$(md5sum "$D/$f" | awk '{print $1}')
      if [ "$got" = "$sum" ]; then echo "  ok        $f"; else
        echo "  MISMATCH  $f (manifest $sum, on disk $got)"
        echo "SUMMARY: installed file $D/$f differs from the bundle's manifest (corrupt upload or extraction)" >> $SUM
      fi
    else
      echo "  present   $f"
    fi
  done < $D/MANIFEST.md5
else
  echo "  $D/MANIFEST.md5 does not exist: the bundle did not get as far as extracting"
  echo "SUMMARY: $D/MANIFEST.md5 is missing: the bundle stopped before or during extraction" >> $SUM
fi
echo
for p in qcarcam_dispatcher qcarcam_injector qcarcam_receiver qcarcam_viewer; do
  echo "--- $p ---"
  if [ ! -e $D/bin/$p ]; then
    echo "  MISSING $D/bin/$p"
    echo "SUMMARY: $p is not installed ($D/bin/$p is missing)" >> $SUM
    echo
    continue
  fi
  ls -l $D/bin/$p
  RC=0
  OUT=$(LD_LIBRARY_PATH="$LD" $D/bin/$p --help </dev/null 2>&1) || RC=$?
  echo "  started with --help: exit code $RC; its output (the loader's message when it cannot load):"
  echo "$OUT" | head -n 40 | sed 's/^/    /'
  echo "  libraries it resolves to (ldd):"
  LD_LIBRARY_PATH="$LD" ldd $D/bin/$p 2>&1 | head -n 40 | sed 's/^/    /'
  echo "$OUT" | sed -n 's/.*unknown symbol:[ ]*\([^ ]*\) referenced from \([^ ]*\).*/\1 \2/p' | sort -u | while read sym from; do
    FOUND=""
    OLDIFS=$IFS
    IFS=:
    for d in $LD; do
      for f in $d/*.so*; do
        if [ -f "$f" ] && grep -q "$sym" "$f" 2>/dev/null; then FOUND="$FOUND $f"; fi
      done
    done
    IFS=$OLDIFS
    if [ -z "$FOUND" ]; then
      echo "  symbol $sym (needed by $from): not found in any library of the search path"
      echo "SUMMARY: $p cannot load: symbol '$sym' (needed by $from) is in no library of the search path. This board's BSP lacks the library or the version that provides it; see the BSP version at the top of this log." >> $SUM
    else
      echo "  symbol $sym (needed by $from): the name appears in:$FOUND"
      echo "SUMMARY: $p cannot load: symbol '$sym' (needed by $from) is not resolved. The name appears only in:$FOUND (these may reference it rather than define it, or be an older version without it)." >> $SUM
    fi
  done
  echo "$OUT" | awk '{for(i=1;i<=NF;i++) if ($i ~ /^lib[^ ]*[.]so/) print $i}' | sed 's/[^A-Za-z0-9_.+-]//g' | sort -u | while read lib; do
    FOUND=""
    OLDIFS=$IFS
    IFS=:
    for d in $LD; do
      if [ -e "$d/$lib" ]; then FOUND="$FOUND $d/$lib"; fi
    done
    IFS=$OLDIFS
    if [ -z "$FOUND" ]; then
      echo "  library $lib (named in the loader's message): NOT FOUND in any search directory"
      echo "SUMMARY: $p cannot load: library $lib is not in any search directory (missing from this board's BSP or the bundle)." >> $SUM
    else
      echo "  library $lib (named in the loader's message): found at$FOUND"
    fi
  done
  if [ "$RC" != 0 ] && ! grep -q "$p" $SUM 2>/dev/null; then
    echo "SUMMARY: $p exited with code $RC when started with --help; see its output in this log." >> $SUM
  fi
  echo
done
echo "--- installed tree ---"
ls -l $D $D/bin $D/lib 2>&1
echo
echo "--- SUMMARY ---"
if [ -s $SUM ]; then cat $SUM; else echo "SUMMARY: no missing file, library or symbol was found; read the sections above."; fi
} > $LOG 2>&1
rm -f $SUM
)SH";

} // namespace

QString TargetDiagnostics::script(const QString& installDir, const QString& bundleName) {
    QString s = QString::fromUtf8(kScript);
    s.replace("@DIR@", installDir);
    s.replace("@LOGNAME@", kLogName);
    s.replace("@BUNDLE@", bundleName);
    return s;
}

TargetDiagnostics::Result TargetDiagnostics::collect(TargetSsh& ssh, const QString& target,
                                                      const QString& bundleName) {
    Result out;
    out.path = QString("%1/tmp/%2").arg(kInstallDir, kLogName);
    installLog("install", "diagnostics: writing " + out.path + " on the target");
    TargetSsh::Result w = ssh.run(target, script(kInstallDir, bundleName), 90000);
    if (!w.ok()) {
        installLog("install", QString("diagnostics: writing the log failed (exit %1): %2")
                                  .arg(w.exitCode).arg(w.stdErr.trimmed()));
        return out;
    }
    TargetSsh::Result r = ssh.run(target, "cat " + out.path, 15000);
    if (!r.ok() || r.stdOut.trimmed().isEmpty()) {
        installLog("install", QString("diagnostics: reading the log back failed (exit %1): %2")
                                  .arg(r.exitCode).arg(r.stdErr.trimmed()));
        return out;
    }
    out.written = true;
    out.text = r.stdOut;
    for (const QString& line : r.stdOut.split('\n')) {
        if (line.startsWith("SUMMARY:")) out.summary << line.trimmed();
    }
    installLog("install", "diagnostics (also in " + out.path + " on the target):\n" + out.text.trimmed());
    return out;
}

} // namespace camsyringe
