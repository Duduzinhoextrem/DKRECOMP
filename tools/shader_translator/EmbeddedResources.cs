// The standalone translator embeds only shader helper text; the rest of Ryujinx.Common is unnecessary.
using System;
using System.IO;
using System.Reflection;

namespace Ryujinx.Common
{
    public static class EmbeddedResources
    {
        public static string ReadAllText(string name)
        {
            using Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(name.Replace('/', '.'));
            if (stream == null) throw new FileNotFoundException("Embedded shader helper is missing", name);
            using StreamReader reader = new(stream);
            return reader.ReadToEnd();
        }
    }
}
