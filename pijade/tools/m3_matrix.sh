#!/bin/sh
# In the container: run the harness for each case m3_matrix.py writes to /probe and find the expected log line.
# Usage: sh /jade/pijade/tools/m3_matrix.sh
PASS=0; FAIL=0
while IFS='|' read -r name exp; do
  sh /jade/pijade/tools/m3_scan.sh "mx_$name" "camfile:/probe/mx_$name.gray:12" > /tmp/mx_$name.harness 2>&1
  DET=$(grep -c "Detected 1 QR codes" /tmp/m3_mx_$name.log)
  if grep -q "$exp" /tmp/m3_mx_$name.log; then PASS=$((PASS+1)); R=PASS; else FAIL=$((FAIL+1)); R=FAIL; fi
  LINES=$(grep -h 'Mining\|mining\|Failed' /tmp/m3_mx_$name.log | sed 's/^.*:\([0-9]*\): //' | tr '\n' ';')
  printf '%-9s detected=%s %s  log: %s\n' "$name" "$DET" "$R" "$LINES"
done < /probe/m3_matrix.txt
echo "MATRIX PASS=$PASS FAIL=$FAIL"
