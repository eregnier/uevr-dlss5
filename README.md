# DLSS 5 <> UEVR : Neural Reconstruction pour Unreal Engine VR

[![Platform: Windows 64-bit](https://img.shields.io/badge/Platform-Windows%20x64-blue.svg)](https://github.com/eregnier/uevr-dlss5/releases)
[![Graphics: DirectX 12](https://img.shields.io/badge/Graphics-DirectX%2012-brightgreen.svg)]()
[![VR: OpenXR / SteamVR](https://img.shields.io/badge/VR-OpenXR%20%7C%20SteamVR-orange.svg)]()
[![Framework: Praydog UEVR](https://img.shields.io/badge/Framework-Praydog%20UEVR-purple.svg)](https://github.com/praydog/UEVR)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

Pont universel et suite logicielle adaptant **NVIDIA DLSS 5 Neural Reconstruction** (moteur OptiScaler Pre-SR Multipass) pour les jeux Unreal Engine 4 et 5 injectés avec **UEVR (Praydog Universal VR Mod)**, avec intégration complète du menu dans le casque en Réalité Virtuelle (OpenXR et OpenVR).

---

## 🚀 Démarrage Rapide (Pour les Joueurs)

> [!TIP]
> **Aucune compilation n'est requise !**  
> Si vous souhaitez simplement jouer, téléchargez l'archive prête à l'emploi dans la section [**Releases GitHub**](https://github.com/eregnier/uevr-dlss5/releases).

### 1. Téléchargement & Préparation
1. Rendez-vous sur les [**Releases du projet**](https://github.com/eregnier/uevr-dlss5/releases) et téléchargez la dernière archive `VR-DLSS5-UEVR-Release.zip`.
2. Extrayez le contenu de l'archive dans un dossier de votre choix.
3. Procurez-vous le runtime NVIDIA DLSS 5 (`nvngx_dlssnr.dll`, build 310.8+) via le SDK officiel NVIDIA ou la communauté.

### 2. Installation en 1 Clic avec l'Installateur Graphique
1. Lancez **`VR-DLSS5-UEVR-Installer.exe`** situé dans le dossier extrait.
2. **Sélectionnez le jeu** :
   - Cliquez sur **Browse...** (ou glissez-déposez l'exécutable du jeu).
   - *Détection intelligente* : Si vous sélectionnez le launcher à la racine du jeu, l'installateur trouve automatiquement le véritable exécutable Unreal Engine dans `Binaries\Win64`.
3. **Sélectionnez votre modèle DLSS 5 (`nvngx_dlssnr.dll`)** :
   - Cliquez sur **Select file...** et sélectionnez votre `nvngx_dlssnr.dll`.
   - L'installateur vérifie automatiquement la signature cryptographique (SHA-256) et mémorise l'emplacement du fichier pour vos futures installations.
4. **Cliquez sur `Install / Update DLSS 5`** :
   - L'installateur s'occupe de tout : injection du runtime `OptiScaler.dll` dans le dossier du jeu, configuration VR sur-mesure (`OptiScaler.ini`), et déploiement du plugin UEVR (`VRDLSS5_UEVR_Plugin.dll`) ainsi que de l'interface en jeu (`VRDLSS5.lua`).
   - **Isolation stricte par jeu** : Le plugin est déployé spécifiquement dans le profil du jeu (`%APPDATA%\UnrealVRMod\<Jeu>\`), évitant ainsi de polluer vos autres jeux UEVR.

### 3. Lancer et Activer en Réalité Virtuelle
1. Démarrez votre casque VR (Meta Quest Link/Virtual Desktop, Valve Index, Bigscreen Beyond, etc.).
2. Lancez le jeu via Steam / Epic Games / etc.
3. Injectez UEVR avec `UEVRInjector.exe` comme d'habitude.
4. Une fois dans le jeu :
   - Ouvrez le menu UEVR en appuyant sur **`L3 + R3`** (clic simultané des deux sticks manette) ou sur la touche **`Insert`** du clavier.
   - Cliquez sur l'onglet **`DLSS 5 Neural Reconstruction`**.
   - Cochez la case **`Enable Neural Engine`** pour activer la reconstruction neuronale en temps réel !

---

## 💡 Principes & Architecture Technique

### 1. Pourquoi le mode Pre-SR Multipass est indispensable en VR ?
- **L'approche naïve** (Post-SR) applique le modèle neuronal DLSS 5 sur l'image stéréo 4K déjà upscalée (~29.5 mégapixels pour les deux yeux). Cette passe coûte 8 à 15 ms sur GPU, ce qui détruit le budget de temps de trame VR (8.3 ms à 120 Hz, 11.1 ms à 90 Hz, 13.8 ms à 72 Hz) et provoque un décrochage immédiat en reprojection.
- **L'architecture Pre-SR Multipass** inverse le pipeline : DLSS 5 est exécuté directement sur le buffer de couleur actif à l'échelle de rendu interne (`WorkingScale = 0.75x` ou `0.66x`), traitant seulement **~4.15 mégapixels** (~0.7 à 1.6 ms sur RTX 4090/5090). L'upscaler DLSS Super Resolution prend ensuite le relais pour agrandir l'image enrichie vers la résolution cible du casque.

### 2. Préservation Intégrale du HUD en Jeu
Dans UEVR, l'interface utilisateur Unreal Engine (Slate / UMG : réticule, boussole, santé, inventaires) est interceptée dans une texture séparée (`ui_target`) puis projetée en 3D dans le casque. Comme cette composition s'effectue **après** le traitement DLSS 5, le HUD conserve une netteté vectorielle native 1:1, **sans aucun flou, ghosting ni déformation**.

### 3. Moteur Neuronal Désactivé par Défaut
> [!IMPORTANT]
> **Le moteur neuronal DLSS 5 est volontairement inactif par défaut (`Enabled = false`) lors de l'installation.**  
> Cela garantit que le jeu se lance avec 100 % de ses performances d'origine sans charge GPU imprévue. Vous l'activez à la demande dans le casque via le menu UEVR.

### 4. Compatibilité DirectX 12 / DirectX 11
- Le modèle neuronal NVIDIA DLSS 5 (`nvngx_dlssnr.dll`) requiert l'architecture **DirectX 12** ou **Vulkan** (calcul tensoriel FP4/FP8 asynchrone).
- Pour les jeux Unreal Engine qui se lancent par défaut en DirectX 11, ajoutez l'argument **`-dx12`** ou **`-d3d12`** dans les options de lancement Steam du jeu pour basculer sur le moteur de rendu DirectX 12.

---

## 🎮 Contrôles & Raccourcis en Jeu

| Action | Raccourci Manette / VR | Raccourci Clavier |
| :--- | :--- | :--- |
| **Ouvrir le menu UEVR** | **`L3 + R3`** (Clic simultané des deux sticks) | **`Insert`** |
| **Accéder aux réglages DLSS 5** | Onglet **`DLSS 5 Neural Reconstruction`** | Même onglet |
| **Ouvrir l'overlay autonome (Direct)** | - | **`F6`** ou **`Home`** |
| **Fermer l'overlay autonome** | - | **`Escape`** ou **`F6`** |
| **Bascule rapide Ray Reconstruction** | - | **`F8`** |

> [!NOTE]
> Aucun hook manette custom n'est actif en tâche de fond. Le plugin s'appuie à 100 % sur l'interception native de UEVR (`L3 + R3`), garantissant **0 % de surcharge CPU** sur le thread de rendu.

---

## ⚙️ Paramètres du Menu DLSS 5

| Section | Paramètre | Valeur par défaut | Description & Recommandation |
| :--- | :--- | :--- | :--- |
| **En-tête** | **Enable Neural Engine** | `Désactivé` | Active ou désactive le traitement neuronal DLSS 5 en temps réel. |
| **Général** | **VR WorkingScale** | `0.75x` | Échelle d'inférence neuronale. Réduire à `0.66x` ou `0.50x` sur les scènes lourdes ou les casques à haute fréquence (90/120 Hz). |
| **Général** | **Run Before SR** | `Activé` | **Indispensable en VR.** Applique DLSS 5 avant l'upscaling pour diviser la surface de calcul par 4. |
| **Général** | **ResidualAcrossRR** | `Désactivé` | Préservation du résidu avec Ray Reconstruction. Laisser désactivé sauf si le jeu utilise activement le Ray Reconstruction. |
| **Général** | **AI Model Preset** | `2 - Performance` | Préréglage du réseau. Le mode 2 (Performance) offre le meilleur ratio netteté / temps de calcul en VR. |
| **Général** | **DLSS 5 Style** | `0 - Standard` | Style de rendu (0: Standard, 1: Naturel, 2: Cinématique). |
| **Général** | **DLSS 5 Intensity** | `1.00x` | Intensité de la reconstruction des micro-détails (0.00x à 2.00x). |
| **Avancé** | **Structure Décor** | `1.00x` | Accentuation des surfaces et éléments géométriques de l'environnement. |
| **Avancé** | **Tonalité Ombres** | `0.00x` | Équilibrage des zones d'ombres et des reflets spéculaires. |
| **Avancé** | **Masque Auto** | `Activé` | Masquage sémantique automatique des personnages pour éviter toute distorsion faciale. |
| **Avancé** | **Structure Peau** | `-1.00x` | Adoucissement et préservation des textures de peau (-1.00 = automatique). |
| **Avancé** | **Passes IA** | `1 - Simple Passe`| Nombre de passes en chaîne. Laisser sur 1 en VR pour préserver le framerate. |
| **Sécurité** | **Dynamic VR Frame Guard** | `Activé` | Surveille le budget de trame GPU en continu. Si le temps dépasse la limite VR, l'échelle est automatiquement ajustée pour maintenir les FPS. |

---

## 🔄 Désinstallation & Restauration

Pour retirer DLSS 5 d'un jeu sans laisser de traces :
1. Lancez **`VR-DLSS5-UEVR-Installer.exe`**.
2. Sélectionnez l'exécutable du jeu.
3. Cliquez sur **`Restore Original`**.  
   L'installateur supprime les DLLs injectées (`OptiScaler.dll`, `VRDLSS5_UEVR_Plugin.dll`, `VRDLSS5.lua`, etc.) et restaure l'état d'origine du jeu.

---

## 🛠️ Compilation Depuis les Sources (Développeurs)

### Prérequis
- Windows 10/11 x64
- Visual Studio 2022 avec les composants C++ (MSVC `cl.exe` v143+ avec support C++20).

### Build Global en Un Clic
```cmd
cd uevr-dlss5
package_release.bat
```
Ce script compile l'ensemble des modules et génère automatiquement l'archive de distribution prête à être déployée :
- `dist/VR-DLSS5-UEVR-Release/`
- `dist/VR-DLSS5-UEVR-Release.zip`

### Compilation Individuelle des Modules
- **Plugin UEVR** :
  ```cmd
  cd plugin
  build.bat
  ```
- **Installateur Graphique** :
  ```cmd
  cd installer
  build.bat
  ```
- **Dual-Proxy Standalone** :
  ```cmd
  cd proxy
  build.bat
  ```

---

## 📂 Organisation du Dépôt

```
uevr-dlss5/
├── installer/             # Installateur Win32 GUI autonome (installer.cpp, build.bat)
├── plugin/                # Plugin C++ UEVR (VRDLSS5_Plugin.cpp, build.bat)
├── scripts/               # Script Lua pour l'interface native UEVR (VRDLSS5.lua)
├── proxy/                 # Dual-proxy optionnel (DXGI + OpenVR Overlay)
├── deps/                  # Moteur OptiScaler Pre-SR, licences tierces et OptiScaler.ini
├── dist/                  # Packages de release générés par package_release.bat
├── doc/                   # Documentation technique détaillée d'architecture
├── package_release.bat    # Script de build et packaging automatisé
├── LICENSE                # Licence MIT + attributions tierces
├── VERSION                # Fichier de version du projet
└── README.md              # Documentation principale
```

---

## ⚖️ Mentions Légales & Remerciements
- Ce projet est un mod indépendant sous licence MIT et n'est pas affilié à NVIDIA, Epic Games ou Valve Corporation.
- Le runtime NVIDIA Neural Rendering (`nvngx_dlssnr.dll`) est la propriété exclusive de NVIDIA Corporation et n'est pas redistribué par ce projet. L'utilisateur doit se le procurer par ses propres moyens selon les termes de NVIDIA.
- **Praydog UEVR** est développé par [praydog](https://github.com/praydog/UEVR) sous licence MIT.
- **OptiScaler** est développé par [OptiScaler contributors](https://github.com/cdozdil/OptiScaler) et le fork DLSS-NR Pre-SR par [wilsjo2](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) sous licence GPL-3.0.
