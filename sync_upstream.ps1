# Lyra-SDR-cpp -- Sync pe5jw fork met N8SDR1 upstream + merge conflict helper
# Draai vanuit C:\project\lyra\Lyra-SDR-cpp
# powershell -ExecutionPolicy Bypass -File .\sync_upstream.ps1

$ErrorActionPreference = "Continue"

function Write-Step { param($msg) Write-Host "" ; Write-Host "--> $msg" -ForegroundColor Cyan }
function Write-Ok   { param($msg) Write-Host "  [OK] $msg" -ForegroundColor Green }
function Write-Warn { param($msg) Write-Host "  [!!] $msg" -ForegroundColor Yellow }
function Write-Err  { param($msg) Write-Host "  [XX] $msg" -ForegroundColor Red }
function Write-Info { param($msg) Write-Host "       $msg" -ForegroundColor Gray }

$repoDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not (Test-Path (Join-Path $repoDir ".git"))) {
    Write-Err "Draai dit script vanuit C:\project\lyra\Lyra-SDR-cpp"
    exit 1
}
Push-Location $repoDir

# Onze 5 gepatChte bestanden
$patchedFiles = @(
    "src\prefs.h",
    "src\prefs.cpp",
    "src\tci_server.h",
    "src\tci_server.cpp",
    "src\settingsdialog.cpp"
)

# Onze patch-markers om te herkennen na merge
$patchMarkers = @(
    "tciRestoreMicSource",
    "preMicSource_",
    "pe5jw patch"
)

Write-Host ""
Write-Host "===================================================" -ForegroundColor Magenta
Write-Host "   Lyra fork sync met N8SDR1 upstream             " -ForegroundColor Magenta
Write-Host "===================================================" -ForegroundColor Magenta

# -- Stap 1: upstream remote toevoegen -------------------------------
Write-Step "Upstream remote controleren"
$remotes = git remote 2>$null
if ($remotes -notcontains "upstream") {
    git remote add upstream https://github.com/N8SDR1/Lyra-SDR-cpp
    Write-Ok "upstream remote toegevoegd"
} else {
    Write-Ok "upstream remote bestaat al"
}

# -- Stap 2: fetch upstream ------------------------------------------
Write-Step "Upstream ophalen (git fetch upstream)"
git fetch upstream 2>&1 | ForEach-Object { Write-Info $_ }

# -- Stap 3: hoeveel commits achter? ---------------------------------
Write-Step "Status controleren"
$behind = git log main..upstream/main --oneline 2>$null
$ahead  = git log upstream/main..main --oneline 2>$null

if ($behind) {
    $behindCount = ($behind | Measure-Object).Count
    Write-Warn "Fork loopt $behindCount commits achter op upstream:"
    $behind | ForEach-Object { Write-Info "  $_" }
} else {
    Write-Ok "Fork is up-to-date met upstream!"
    Pop-Location
    exit 0
}

if ($ahead) {
    $aheadCount = ($ahead | Measure-Object).Count
    Write-Info ""
    Write-Info "Fork heeft $aheadCount eigen commits (onze patch):"
    $ahead | ForEach-Object { Write-Info "  $_" }
}

# -- Stap 4: backup van gepatChte bestanden --------------------------
Write-Step "Backup van gepatChte bestanden maken"
$backupDir = Join-Path $repoDir "patch_backup_$(Get-Date -Format 'yyyyMMdd_HHmmss')"
New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
foreach ($f in $patchedFiles) {
    $src = Join-Path $repoDir $f
    $dst = Join-Path $backupDir ($f -replace "\\","_")
    if (Test-Path $src) {
        Copy-Item $src $dst
        Write-Ok "Backup: $f"
    }
}
Write-Info "Backup opgeslagen in: $backupDir"

# -- Stap 5: merge uitvoeren ------------------------------------------
Write-Step "Merge uitvoeren (git merge upstream/main)"
$mergeOutput = git merge upstream/main -m "Sync met N8SDR1/Lyra-SDR-cpp upstream" 2>&1
$mergeOutput | ForEach-Object { Write-Info $_ }
$mergeExit = $LASTEXITCODE

if ($mergeExit -eq 0) {
    Write-Ok "Merge geslaagd zonder conflicten!"

    # Controleer of onze patch nog intact is
    Write-Step "Patch integriteit controleren"
    $patchOk = $true
    foreach ($f in $patchedFiles) {
        $fullPath = Join-Path $repoDir $f
        if (Test-Path $fullPath) {
            $content = Get-Content $fullPath -Raw
            $hasMarker = $false
            foreach ($marker in $patchMarkers) {
                if ($content -like "*$marker*") { $hasMarker = $true ; break }
            }
            if ($hasMarker) {
                Write-Ok "$f -- patch intact"
            } else {
                Write-Warn "$f -- patch MOGELIJK OVERSCHREVEN door upstream"
                $patchOk = $false
            }
        }
    }

    if (-not $patchOk) {
        Write-Host ""
        Write-Warn "Sommige patch-markers niet gevonden na merge."
        Write-Warn "Patch opnieuw toepassen via:"
        Write-Host "  powershell -ExecutionPolicy Bypass -File ..\apply_patch.ps1" -ForegroundColor Yellow
    }

    Write-Step "Pushen naar pe5jw fork"
    git push origin main 2>&1 | ForEach-Object { Write-Info $_ }
    if ($LASTEXITCODE -eq 0) {
        Write-Ok "Fork bijgewerkt op GitHub!"
    } else {
        Write-Err "Push mislukt. Probeer handmatig: git push origin main"
    }

} else {
    # -- Conflicten afhandelen ----------------------------------------
    Write-Host ""
    Write-Host "===================================================" -ForegroundColor Red
    Write-Host "   MERGE CONFLICTEN GEVONDEN                       " -ForegroundColor Red
    Write-Host "===================================================" -ForegroundColor Red

    $conflictFiles = git diff --name-only --diff-filter=U 2>$null
    Write-Host ""
    Write-Warn "Conflicten in:"
    $conflictFiles | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }

    # Splits conflicten in onze bestanden vs andere bestanden
    $ourConflicts   = @()
    $otherConflicts = @()
    foreach ($cf in $conflictFiles) {
        $cfWin = $cf -replace "/","\"
        if ($patchedFiles -contains $cfWin) {
            $ourConflicts += $cf
        } else {
            $otherConflicts += $cf
        }
    }

    # Andere conflicten: upstream versie accepteren
    if ($otherConflicts.Count -gt 0) {
        Write-Step "Niet-gepatChte conflictbestanden: upstream versie accepteren"
        foreach ($cf in $otherConflicts) {
            git checkout --theirs $cf 2>$null
            git add $cf 2>$null
            Write-Ok "Upstream versie: $cf"
        }
    }

    # Onze conflicten: upstream basis + patch opnieuw toepassen
    if ($ourConflicts.Count -gt 0) {
        Write-Step "GepatChte bestanden: upstream basis nemen + markers controleren"
        foreach ($cf in $ourConflicts) {
            $cfWin = $cf -replace "/","\"
            # Neem upstream versie als basis
            git checkout --theirs $cf 2>$null
            git add $cf 2>$null
            Write-Warn "$cf -- upstream versie genomen, patch moet opnieuw toegepast worden"
        }

        Write-Host ""
        Write-Host "===================================================" -ForegroundColor Yellow
        Write-Host "   ACTIE VEREIST                                    " -ForegroundColor Yellow
        Write-Host "===================================================" -ForegroundColor Yellow
        Write-Host ""
        Write-Host "  De volgende bestanden zijn bijgewerkt naar upstream" -ForegroundColor Yellow
        Write-Host "  maar onze patch moet opnieuw worden toegepast:" -ForegroundColor Yellow
        Write-Host ""
        $ourConflicts | ForEach-Object { Write-Host "    - $_" -ForegroundColor Yellow }
        Write-Host ""
        Write-Host "  Stap 1: Merge afronden" -ForegroundColor Cyan
        Write-Host "    git commit -m `"Sync upstream + patch opnieuw toepassen`""
        Write-Host ""
        Write-Host "  Stap 2: Patch opnieuw toepassen" -ForegroundColor Cyan
        Write-Host "    powershell -ExecutionPolicy Bypass -File ..\apply_patch.ps1"
        Write-Host ""
        Write-Host "  Stap 3: Committen en pushen" -ForegroundColor Cyan
        Write-Host "    git add src\prefs.h src\prefs.cpp src\tci_server.h src\tci_server.cpp src\settingsdialog.cpp"
        Write-Host "    git commit -m `"Re-apply TCI mic-restore patch after upstream sync`""
        Write-Host "    git push origin main"
        Write-Host ""

        $keuze = Read-Host "  Merge nu afronden en apply_patch.ps1 automatisch uitvoeren? (j/n)"
        if ($keuze -eq "j" -or $keuze -eq "J") {
            git commit -m "Sync met N8SDR1 upstream + patch opnieuw toepassen" 2>&1 | ForEach-Object { Write-Info $_ }

            $applyPatch = Join-Path (Split-Path $repoDir -Parent) "apply_patch.ps1"
            if (Test-Path $applyPatch) {
                Write-Step "apply_patch.ps1 uitvoeren"
                powershell -ExecutionPolicy Bypass -File $applyPatch
                Write-Step "Gepatcht committen en pushen"
                git add src\prefs.h src\prefs.cpp src\tci_server.h src\tci_server.cpp src\settingsdialog.cpp
                git commit -m "Re-apply TCI mic-restore patch after upstream sync"
                git push origin main 2>&1 | ForEach-Object { Write-Info $_ }
                Write-Ok "Fork bijgewerkt en patch opnieuw toegepast!"
            } else {
                Write-Warn "apply_patch.ps1 niet gevonden op: $applyPatch"
                Write-Warn "Download apply_patch.ps1 opnieuw en voer stap 2 handmatig uit."
                git commit -m "Sync met N8SDR1 upstream (patch nog toepassen)" 2>&1 | ForEach-Object { Write-Info $_ }
                git push origin main 2>&1 | ForEach-Object { Write-Info $_ }
            }
        } else {
            Write-Info "Voer de stappen hierboven handmatig uit."
        }
    } else {
        # Alleen andere conflicten, geen onze bestanden
        Write-Step "Merge afronden"
        git commit -m "Sync met N8SDR1/Lyra-SDR-cpp upstream" 2>&1 | ForEach-Object { Write-Info $_ }
        git push origin main 2>&1 | ForEach-Object { Write-Info $_ }
        Write-Ok "Fork bijgewerkt!"
    }
}

Write-Host ""
Write-Host "===================================================" -ForegroundColor Green
Write-Host "   SYNC VOLTOOID                                   " -ForegroundColor Green
Write-Host "===================================================" -ForegroundColor Green
Write-Host ""
Write-Host "  Opnieuw builden na sync:" -ForegroundColor Cyan
Write-Host "    cd C:\project\lyra"
Write-Host "    powershell -ExecutionPolicy Bypass -File .\build_lyra.ps1"
Write-Host ""

Pop-Location
