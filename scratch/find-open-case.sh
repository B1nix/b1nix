#!/usr/bin/env bash
set -euo pipefail
# Scan configure and find the matching case for the esac at line 68266
awk '
BEGIN {
    nest = 0
}
{
    # Ignore comments
    line = $0
    sub(/#.*/, "", line)
    
    # We want to match whole words "case" and "esac"
    # To keep it simple, look for "case" and "esac" as patterns
    # Count how many times they appear in the line
    n_case = gsub(/\<case\>/, "case", line)
    n_esac = gsub(/\<esac\>/, "esac", line)
    
    for (i = 1; i <= n_case; i++) {
        nest++
        stack[nest] = NR
    }
    for (i = 1; i <= n_esac; i++) {
        if (NR == 68266) {
            print "Matching case is at line " stack[nest]
            exit
        }
        nest--
    }
}
' /root/b1nix-toolchain/src/gcc-13.2.0/libstdc++-v3/configure
