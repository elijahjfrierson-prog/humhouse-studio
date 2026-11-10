#include "UI.h"

// Options > Layout > Language: menus and main labels through juce::translate (English = no mapping)
namespace hhs
{
static const char* const kKeys[] { "File", "Edit", "View", "Options", "Help", "New project", "Open...", "Save (+ backup)", "Save as...",
    "Export (WAV / MP3 / M4A / stems)...", "Quit", "Undo", "Redo", "Audio settings (device, buffer size, sample rate)..." };
static const char* const kLangs[][2] {
    { "Espanol", u8"Archivo|Editar|Ver|Opciones|Ayuda|Nuevo proyecto|Abrir...|Guardar (+ copia)|Guardar como...|Exportar (WAV / MP3 / M4A / pistas)...|Salir|Deshacer|Rehacer|Ajustes de audio (dispositivo, búfer, frecuencia)..." },
    { "Francais", u8"Fichier|Édition|Affichage|Options|Aide|Nouveau projet|Ouvrir...|Enregistrer (+ copie)|Enregistrer sous...|Exporter (WAV / MP3 / M4A / stems)...|Quitter|Annuler|Rétablir|Réglages audio (périphérique, tampon, fréquence)..." },
    { "Portugues", u8"Arquivo|Editar|Exibir|Opções|Ajuda|Novo projeto|Abrir...|Salvar (+ backup)|Salvar como...|Exportar (WAV / MP3 / M4A / stems)...|Sair|Desfazer|Refazer|Configurações de áudio (dispositivo, buffer, taxa)..." },
    { "Deutsch", u8"Datei|Bearbeiten|Ansicht|Optionen|Hilfe|Neues Projekt|Öffnen...|Speichern (+ Backup)|Speichern unter...|Exportieren (WAV / MP3 / M4A / Stems)...|Beenden|Rückgängig|Wiederholen|Audio-Einstellungen (Gerät, Puffer, Samplerate)..." },
    { "Italiano", u8"File|Modifica|Vista|Opzioni|Aiuto|Nuovo progetto|Apri...|Salva (+ backup)|Salva con nome...|Esporta (WAV / MP3 / M4A / stem)...|Esci|Annulla|Ripeti|Impostazioni audio (dispositivo, buffer, frequenza)..." },
    { "Japanese", u8"ファイル|編集|表示|オプション|ヘルプ|新規プロジェクト|開く...|保存 (+ バックアップ)|名前を付けて保存...|書き出し (WAV / MP3 / M4A / ステム)...|終了|元に戻す|やり直し|オーディオ設定 (デバイス、バッファ、サンプルレート)..." },
    { "Korean", u8"파일|편집|보기|옵션|도움말|새 프로젝트|열기...|저장 (+ 백업)|다른 이름으로 저장...|내보내기 (WAV / MP3 / M4A / 스템)...|종료|실행 취소|다시 실행|오디오 설정 (장치, 버퍼, 샘플레이트)..." },
    { "Chinese", u8"文件|编辑|视图|选项|帮助|新建项目|打开...|保存 (+ 备份)|另存为...|导出 (WAV / MP3 / M4A / 分轨)...|退出|撤销|重做|音频设置 (设备、缓冲区、采样率)..." },
    { "Hindi", u8"फ़ाइल|संपादित करें|देखें|विकल्प|सहायता|नया प्रोजेक्ट|खोलें...|सहेजें (+ बैकअप)|इस रूप में सहेजें...|निर्यात (WAV / MP3 / M4A / स्टेम)...|बंद करें|पूर्ववत करें|फिर से करें|ऑडियो सेटिंग्स (डिवाइस, बफ़र, सैंपल रेट)..." } };

const juce::StringArray& languageNames()
{
    static const juce::StringArray n { "English", juce::String::fromUTF8 (u8"Español"), juce::String::fromUTF8 (u8"Français"), juce::String::fromUTF8 (u8"Português"),
        "Deutsch", "Italiano", juce::String::fromUTF8 (u8"日本語"), juce::String::fromUTF8 (u8"한국어"), juce::String::fromUTF8 (u8"中文"), juce::String::fromUTF8 (u8"हिन्दी") };
    return n;
}
void applyLanguage (int i)
{
    if (i <= 0 || i > (int) std::size (kLangs)) { juce::LocalisedStrings::setCurrentMappings (nullptr); return; }
    const auto vals = juce::StringArray::fromTokens (juce::String::fromUTF8 (kLangs[i - 1][1]), "|", "");
    juce::String text = "language: " + juce::String (kLangs[i - 1][0]) + "\n";
    for (int k = 0; k < (int) std::size (kKeys) && k < vals.size(); ++k)
        text << "\"" << kKeys[k] << "\" = \"" << vals[k] << "\"\n";
    juce::LocalisedStrings::setCurrentMappings (new juce::LocalisedStrings (text, false));
}
}
